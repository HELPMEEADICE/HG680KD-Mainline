/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/hex.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "hi3798mv320-vpu-h264-data.h"

#define POC_IOVA		0x10000000U
#define POC_TABLE_SIZE		SZ_4M
#define POC_ARENA_SIZE		SZ_1M
#define POC_PAGE_SIZE		SZ_4K
#define POC_CORE		0x100
#define POC_PICTURE		0x800
#define POC_SLICE		0xe00
#define POC_UP_MSG		0x32e00
#define POC_UP_MSG_SIZE		0x400
#define POC_UP_MSG_RESPONSE	0x100
#define POC_EXT			0x52000
#define POC_CABAC		POC_EXT
#define POC_EXT_WORK		(POC_EXT + 0x1800)
#define POC_STREAM		0x80000
#define POC_OUTPUT		0x90000
#define POC_PMV			0x98000
#define POC_FAULT_READ		0xa0000
#define POC_FAULT_WRITE		0xa0200

#define POC_WIDTH		32
#define POC_HEIGHT		32
#define POC_PIXELS		(POC_WIDTH * POC_HEIGHT)
#define POC_STRIDE		256
#define POC_Y_BYTES		(POC_STRIDE * 64)
#define POC_UV_BYTES		(POC_Y_BYTES / 2)
#define POC_POISON		0xa5

#define POC_SLOT		0x100
#define PXP_BSP_CLEAR		0x04
#define PXP_IRQ_CLEAR		0x14
#define PXP_DECODE_DONE		BIT(17)
#define PXP_DECODE_ERROR	BIT(18)
#define PXP_AUX_DONE		BIT(19)
#define SMMU_IRQ_MASK		0x20
#define SMMU_IRQ_CLEAR		0x2c
#define SMMU_TABLE		0x2ec
#define SMMU_READ_SINK		0x304
#define SMMU_WRITE_SINK		0x30c
#define SMMU_WRITE_FAULT	0x340
#define SMMU_READ_FAULT		0x360

struct h264_poc {
	struct device *dev;
	void __iomem *vdh, *pxp, *global, *scd, *smmu;
	u8 *arena;
	__le32 *table;
	dma_addr_t arena_dma, table_dma;
	struct completion done;
	struct mutex mutex;
	int pxp_irq, smmu_irq;
	u32 status, fault;
	bool failed_reset, frame_valid;
};

static u32 h264_addr(unsigned int offset)
{
	return (POC_IOVA + offset) >> 4;
}

static void h264_poc_messages(struct h264_poc *poc)
{
	__le32 *pub = (__le32 *)poc->arena;
	__le32 *core = (__le32 *)(poc->arena + POC_CORE);
	__le32 *pic = (__le32 *)(poc->arena + POC_PICTURE);
	__le32 *slice = (__le32 *)(poc->arena + POC_SLICE);
	u32 yuv_end = POC_OUTPUT + POC_Y_BYTES + POC_UV_BYTES;
	unsigned int i;

	memset(poc->arena, 0, POC_FAULT_WRITE + 512);
	memcpy(poc->arena + POC_STREAM, h264_test_stream,
	       sizeof(h264_test_stream));
	memcpy(poc->arena + POC_CABAC, h264_cabac_init,
	       sizeof(h264_cabac_init));
	memset(poc->arena + POC_UP_MSG, POC_POISON, POC_UP_MSG_SIZE);
	memset(poc->arena + POC_OUTPUT, POC_POISON, POC_Y_BYTES + POC_UV_BYTES);

	pub[0] = cpu_to_le32(h264_addr(POC_UP_MSG));
	pub[2] = cpu_to_le32(0x41000003);
	pub[3] = cpu_to_le32(0x00030000);
	pub[4] = cpu_to_le32(h264_addr(POC_PICTURE));
	pub[6] = cpu_to_le32(h264_addr(POC_STREAM));
	pub[8] = cpu_to_le32(1);
	pub[9] = cpu_to_le32(h264_addr(POC_OUTPUT));
	pub[10] = cpu_to_le32(POC_STRIDE);
	pub[12] = cpu_to_le32(POC_Y_BYTES);
	pub[20] = cpu_to_le32(h264_addr(yuv_end));
	pub[22] = cpu_to_le32(h264_addr(POC_EXT_WORK + 0x6000));
	pub[23] = cpu_to_le32(h264_addr(POC_PMV));
	pub[24] = cpu_to_le32(h264_addr(POC_EXT_WORK + 0xe000));
	pub[25] = cpu_to_le32(h264_addr(POC_CABAC));
	pub[30] = cpu_to_le32(h264_addr(POC_EXT_WORK + 0x1e000));
	pub[31] = cpu_to_le32(0x00010001);
	pub[48] = cpu_to_le32(POC_UP_MSG_SIZE);

	/* Progressive 4:2:0 I picture, no reference history, flat scaling. */
	pic[0] = cpu_to_le32(0x00014001);
	for (i = 64; i < 120; i++)
		pic[i] = cpu_to_le32(0x10101010);

	/* Annex B byte 41 bit 6 is the start of CAVLC slice_data(). */
	slice[0] = cpu_to_le32(41 / 16);
	slice[1] = cpu_to_le32((41 % 16) * 8 + 6);
	slice[2] = cpu_to_le32(134);
	slice[6] = cpu_to_le32(5U << 26);
	slice[10] = cpu_to_le32((30U << 5) | 30U);
	slice[11] = 0;
	slice[0xb0 / 4] = cpu_to_le32(3);
	slice[0xf8 / 4] = cpu_to_le32(0x00010001);

	for (i = 0; i < 16; i++) {
		core[0x500 / 4 + i] = cpu_to_le32(h264_addr(POC_OUTPUT));
		core[0x540 / 4 + i] = cpu_to_le32(h264_addr(yuv_end));
		core[0x580 / 4 + i] = cpu_to_le32(h264_addr(POC_OUTPUT));
	}
	for (i = 0; i < 3; i++) {
		core[0x5cc / 4 + i] = cpu_to_le32(0x00020020);
		core[0x5d8 / 4 + i] = cpu_to_le32(POC_Y_BYTES);
		core[0x5e4 / 4 + i] = 0;
		core[0x5fc / 4 + i] = cpu_to_le32(POC_STRIDE);
	}
	core[0x614 / 4] = cpu_to_le32(h264_addr(yuv_end));
	core[0x618 / 4] = 0;
	core[0x61c / 4] = cpu_to_le32(h264_addr(POC_OUTPUT));
}

static u32 h264_pxp_clear(u32 status)
{
	return (status & PXP_DECODE_DONE ? BIT(0) : 0) |
	       (status & PXP_AUX_DONE ? BIT(2) : 0);
}

static irqreturn_t h264_pxp_irq(int irq, void *data)
{
	struct h264_poc *poc = data;
	u32 status = readl(poc->pxp + VDH_IRQ_STATUS);
	u32 clear = h264_pxp_clear(status);
	u32 mb, info, cycles;

	if (!clear)
		return IRQ_NONE;

	mb = readl(poc->pxp + 0x20);
	info = readl(poc->pxp + 0x24);
	cycles = readl(poc->pxp + 0x28);
	writel(clear, poc->pxp + PXP_IRQ_CLEAR);
	dev_info(poc->dev, "PXP IRQ %d status=%08x +20=%08x +24=%08x +28=%08x\n",
		 irq, status, mb, info, cycles);
	if (status & PXP_DECODE_DONE) {
		WRITE_ONCE(poc->status, status);
		complete(&poc->done);
	}
	return IRQ_HANDLED;
}

static u32 h264_poc_fault(struct h264_poc *poc)
{
	u32 status = readl(poc->smmu + SMMU_IRQ_STATUS);

	if (status) {
		dev_err(poc->dev, "SMMU status=%08x read=%08x write=%08x\n",
			status, readl(poc->smmu + SMMU_READ_FAULT),
			readl(poc->smmu + SMMU_WRITE_FAULT));
		writel(0xf, poc->smmu + SMMU_IRQ_CLEAR);
	}
	return status;
}

static irqreturn_t h264_smmu_irq(int irq, void *data)
{
	struct h264_poc *poc = data;
	u32 status = h264_poc_fault(poc);

	if (!status)
		return IRQ_NONE;
	WRITE_ONCE(poc->fault, status);
	complete(&poc->done);
	return IRQ_HANDLED;
}

static irqreturn_t h264_scd_irq(int irq, void *data)
{
	struct h264_poc *poc = data;
	u32 status = readl(poc->scd + SCD_IRQ_STATUS);

	if (!(status & 1))
		return IRQ_NONE;
	writel(1, poc->scd + SCD_IRQ_CLEAR);
	dev_warn(poc->dev, "unexpected SCD IRQ %d status=%08x\n", irq, status);
	return IRQ_HANDLED;
}

static void h264_poc_dump_nz(struct device *dev, const char *tag,
			     const void __iomem *base, unsigned int size)
{
	unsigned int off;

	for (off = 0; off < size; off += 16) {
		u32 a = readl(base + off);
		u32 b = readl(base + off + 4);
		u32 c = readl(base + off + 8);
		u32 d = readl(base + off + 12);

		if (a | b | c | d)
			dev_info(dev, "%s +%03x: %08x %08x %08x %08x\n",
				tag, off, a, b, c, d);
	}
}

static void h264_poc_dump_diag(struct h264_poc *poc, bool decode_error)
{
	const u8 *up = poc->arena + POC_UP_MSG;
	const u8 *wr = poc->arena + POC_FAULT_WRITE;
	const u8 *y = poc->arena + POC_OUTPUT;
	const u8 *uv = y + POC_Y_BYTES;
	bool wr_sink_nz = memchr_inv(wr, 0, 512);
	bool y_mod = memchr_inv(y, POC_POISON, POC_Y_BYTES);
	bool uv_mod = memchr_inv(uv, POC_POISON, POC_UV_BYTES);
	bool up_tail_mod = memchr_inv(up + POC_UP_MSG_RESPONSE, POC_POISON,
				     POC_UP_MSG_SIZE - POC_UP_MSG_RESPONSE);
	unsigned int i, up_changed = 0;

	for (i = 0; i < POC_UP_MSG_RESPONSE; i++)
		up_changed += up[i] != POC_POISON;
	dev_info(poc->dev,
		 "SMMU+0x28=%08x wr_sink_nz=%d y_mod=%d uv_mod=%d up_iova=%08x up_changed=%u up_tail_mod=%d\n",
		 readl(poc->smmu + SMMU_IRQ_STATUS), wr_sink_nz, y_mod, uv_mod,
		 POC_IOVA + POC_UP_MSG, up_changed, up_tail_mod);
	print_hex_dump(KERN_INFO, "UP_MSG ", DUMP_PREFIX_OFFSET, 16, 1,
		       up, POC_UP_MSG_RESPONSE, false);
	h264_poc_dump_nz(poc->dev, "SMMU", poc->smmu, 0x400);
	h264_poc_dump_nz(poc->dev, "VDH", poc->vdh, 0x140);
	dev_info(poc->dev, "PXP +20=%08x +24=%08x +28=%08x\n",
		 readl(poc->pxp + 0x20), readl(poc->pxp + 0x24),
		 readl(poc->pxp + 0x28));
	if (wr_sink_nz)
		print_hex_dump(KERN_INFO, "WR_SINK ", DUMP_PREFIX_OFFSET, 16, 1,
			       wr, 512, false);
	/* Preserve any partial output when the frame attributes cannot return it. */
	if (y_mod && (decode_error || !uv_mod))
		print_hex_dump(KERN_INFO, "Y_RAW ", DUMP_PREFIX_OFFSET, 32, 1,
			       y, POC_Y_BYTES, false);
	if (uv_mod && (decode_error || !y_mod))
		print_hex_dump(KERN_INFO, "UV_RAW ", DUMP_PREFIX_OFFSET, 32, 1,
			       uv, POC_UV_BYTES, false);
}

static int h264_poc_run(struct h264_poc *poc)
{
	u32 status, fault;
	unsigned long timeout;
	int ret;

	if (poc->failed_reset)
		return -EIO;
	poc->frame_valid = false;
	ret = hi3798mv320_vpu_reset(poc->dev, poc->global, poc->pxp);
	if (ret)
		goto reset_failed;

	h264_poc_messages(poc);
	WRITE_ONCE(poc->status, 0);
	WRITE_ONCE(poc->fault, 0);
	reinit_completion(&poc->done);
	dma_wmb();
	writel(lower_32_bits(poc->table_dma), poc->smmu + SMMU_TABLE);
	writel(lower_32_bits(poc->arena_dma + POC_FAULT_READ), poc->smmu + SMMU_READ_SINK);
	writel(lower_32_bits(poc->arena_dma + POC_FAULT_WRITE), poc->smmu + SMMU_WRITE_SINK);
	writel(8, poc->smmu + SMMU_CTRL);
	writel(0xf, poc->smmu + SMMU_IRQ_CLEAR);
	writel(0, poc->smmu + SMMU_IRQ_MASK);

	writel(0xaaaaaaaa, poc->vdh + 0x00);
	writel(0xaaaaaaaa, poc->vdh + 0x04);
	writel(0x000b0003, poc->vdh + 0x08);
	writel(0x40, poc->vdh + 0x14);
	writel(1, poc->global + 0x08);
	writel(h264_pxp_clear(readl(poc->pxp + VDH_IRQ_STATUS)),
	       poc->pxp + PXP_IRQ_CLEAR);
	writel(1, poc->pxp + PXP_BSP_CLEAR);
	writel(1, poc->pxp + VDH_IRQ_MASK);
	writel(0, poc->pxp + VDH_IRQ_AUX_MASK);
	writel(0x300, poc->vdh + POC_SLOT + 0x04);
	writel(h264_addr(0), poc->vdh + POC_SLOT + 0x08);
	dma_wmb();
	writel(1, poc->vdh + POC_SLOT);
	writel(1, poc->vdh + POC_SLOT + 0x10);

	timeout = wait_for_completion_timeout(&poc->done, msecs_to_jiffies(500));
	dev_info(poc->dev, "slot 0 wait=%lu PXP state=%08x irq=%08x BSP=%08x SCD=%08x SMMU=%08x\n",
		 timeout, readl(poc->pxp + VDH_PXP_STATE),
		 readl(poc->pxp + VDH_IRQ_STATUS), readl(poc->pxp),
		 readl(poc->scd + SCD_IRQ_STATUS),
		 readl(poc->smmu + SMMU_IRQ_STATUS));

	dma_rmb();
	status = READ_ONCE(poc->status);
	fault = READ_ONCE(poc->fault) | readl(poc->smmu + SMMU_IRQ_STATUS);
	h264_poc_dump_diag(poc, fault || !timeout || !(status & PXP_DECODE_DONE) ||
			  (status & PXP_DECODE_ERROR) || (status & 0xffff));

	/* The mappings remain pinned for the lifetime of the built-in driver. */
	ret = hi3798mv320_vpu_reset(poc->dev, poc->global, poc->pxp);
	if (ret)
		goto reset_failed;
	writel(0xf, poc->smmu + SMMU_IRQ_MASK);
	synchronize_irq(poc->pxp_irq);
	synchronize_irq(poc->smmu_irq);
	status = READ_ONCE(poc->status);
	fault = READ_ONCE(poc->fault) | h264_poc_fault(poc);
	dev_info(poc->dev, "slot 0 completion=%08x SMMU faults=%08x\n", status, fault);
	if (fault)
		return -EFAULT;
	if (!timeout)
		return -ETIMEDOUT;
	if (!(status & PXP_DECODE_DONE) || (status & PXP_DECODE_ERROR) ||
	    (status & 0xffff))
		return -EIO;

	if (!memchr_inv(poc->arena + POC_OUTPUT, POC_POISON, POC_Y_BYTES) ||
	    !memchr_inv(poc->arena + POC_OUTPUT + POC_Y_BYTES, POC_POISON, POC_UV_BYTES))
		return -ENODATA;
	poc->frame_valid = true;
	return 0;

reset_failed:
	poc->failed_reset = true;
	dev_crit(poc->dev, "PXP reset failed, DMA pinned until reboot\n");
	return ret;
}

static ssize_t decode_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct h264_poc *poc = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;

	mutex_lock(&poc->mutex);
	ret = h264_poc_run(poc);
	mutex_unlock(&poc->mutex);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(decode);

static ssize_t h264_frame_show(struct device *dev, char *buf, bool tiled)
{
	struct h264_poc *poc = dev_get_drvdata(dev);
	const u8 *y_plane = poc->arena + POC_OUTPUT;
	const u8 *vu_plane = y_plane + POC_Y_BYTES;
	char *pos = buf;
	unsigned int row, col, component, offset;

	mutex_lock(&poc->mutex);
	if (!poc->frame_valid) {
		mutex_unlock(&poc->mutex);
		return -ENODATA;
	}
	for (row = 0; row < POC_HEIGHT; row++) {
		offset = tiled ? (row / 16) * (POC_STRIDE * 16) + (row % 16) * 64 :
				 row * POC_STRIDE;
		for (col = 0; col < POC_WIDTH; col++)
			pos = hex_byte_pack(pos, y_plane[offset + col]);
	}
	for (component = 1; component <= 2; component++) {
		for (row = 0; row < POC_HEIGHT / 2; row++) {
			offset = tiled ? (row / 8) * (POC_STRIDE * 8) + (row % 8) * 64 :
					 row * POC_STRIDE;
			for (col = 0; col < POC_WIDTH / 2; col++)
				pos = hex_byte_pack(pos, vu_plane[offset + 2 * col + 2 - component]);
		}
	}
	*pos++ = '\n';
	mutex_unlock(&poc->mutex);
	return pos - buf;
}

static ssize_t frame_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return h264_frame_show(dev, buf, true);
}
static DEVICE_ATTR_RO(frame);

static ssize_t linear_frame_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return h264_frame_show(dev, buf, false);
}
static DEVICE_ATTR_RO(linear_frame);

static struct attribute *h264_poc_attrs[] = {
	&dev_attr_decode.attr,
	&dev_attr_frame.attr,
	&dev_attr_linear_frame.attr,
	NULL,
};
static const struct attribute_group h264_poc_group = {
	.attrs = h264_poc_attrs,
};

static int hi3798mv320_h264_setup(struct platform_device *pdev, void __iomem *vdh,
				 void __iomem *global, void __iomem *scd, void __iomem *smmu,
				 int pxp_irq, int scd_irq, int smmu_irq)
{
	struct device *dev = &pdev->dev;
	struct h264_poc *poc;
	unsigned int offset;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	poc = devm_kzalloc(dev, sizeof(*poc), GFP_KERNEL);
	if (!poc)
		return -ENOMEM;
	poc->dev = dev;
	poc->vdh = vdh;
	poc->pxp = vdh + VDH_PXP0;
	poc->global = global;
	poc->scd = scd;
	poc->smmu = smmu;
	poc->pxp_irq = pxp_irq;
	poc->smmu_irq = smmu_irq;
	mutex_init(&poc->mutex);
	init_completion(&poc->done);

	poc->table = dmam_alloc_coherent(dev, POC_TABLE_SIZE, &poc->table_dma, GFP_KERNEL);
	poc->arena = dmam_alloc_coherent(dev, POC_ARENA_SIZE, &poc->arena_dma, GFP_KERNEL);
	if (!poc->table || !poc->arena)
		return dev_err_probe(dev, -ENOMEM, "cannot allocate contiguous VPU DMA\n");
	if (!IS_ALIGNED(poc->arena_dma, POC_PAGE_SIZE) ||
	    !IS_ALIGNED(poc->table_dma, POC_PAGE_SIZE))
		return -EINVAL;

	memset(poc->table, 0, POC_TABLE_SIZE);
	for (offset = 0; offset < POC_ARENA_SIZE; offset += POC_PAGE_SIZE)
		poc->table[(POC_IOVA + offset) / POC_PAGE_SIZE] =
			cpu_to_le32(lower_32_bits(poc->arena_dma + offset) | 1);
	dma_wmb();

	ret = devm_request_irq(dev, pxp_irq, h264_pxp_irq, 0, "hg680kd-pxp", poc);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, scd_irq, h264_scd_irq, 0, "hg680kd-scd", poc);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, smmu_irq, h264_smmu_irq, 0, "hg680kd-smmu", poc);
	if (ret)
		return ret;

	dev_set_drvdata(dev, poc);
	ret = devm_device_add_group(dev, &h264_poc_group);
	if (ret)
		return ret;
	dev_info(dev, "fixed H.264 IDR ready, table=%pad arena=%pad IOVA=%08x\n",
		 &poc->table_dma, &poc->arena_dma, POC_IOVA);
	return 0;
}
