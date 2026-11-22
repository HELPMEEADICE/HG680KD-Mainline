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
#define POC_MSG_STRIDE		0x2000
#define POC_UP_MSG		0x32e00
#define POC_UP_MSG_SIZE		0x400
#define POC_UP_MSG_RESPONSE	0x100
#define POC_EXT			0x52000
#define POC_CABAC		POC_EXT
#define POC_EXT_WORK		(POC_EXT + 0x1800)
#define POC_STREAM		0x80000
#define POC_OUTPUT_0		0x90000
#define POC_OUTPUT_1		0x98000
#define POC_PMV_0		0xa0000
#define POC_PMV_1		0xa1000
#define POC_FAULT_READ		0xa2000
#define POC_FAULT_WRITE		0xa2200

#define POC_WIDTH		32
#define POC_HEIGHT		32
#define POC_PIXELS		(POC_WIDTH * POC_HEIGHT)
#define POC_STRIDE		256
#define POC_TILED_Y_BYTES	(POC_STRIDE * 64)
#define POC_LINEAR_Y_BYTES	(POC_STRIDE * POC_HEIGHT)
#define POC_POISON		0xa5
/* Stock 2d_en request. This VDH still writes tiles in the 32x32 probe. */
#define POC_PUB_2D_REQUEST	BIT(29)

#define POC_SLOT		0x100
#define POC_SLOT_STRIDE		0x20
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

struct h264_poc_picture {
	u16 data_offset;
	u16 data_bits;
	u8 bit_offset;
	u8 qp;
	u8 poc;
	u8 cabac_init_idc;
	bool predicted;
};

/* Fixed single-slice vectors. Offsets are into the Annex B stream. */
static const struct h264_poc_picture h264_cavlc_idr = {
	.data_offset = 41, .data_bits = 134, .bit_offset = 6, .qp = 5,
};

static const struct h264_poc_picture h264_cabac_pictures[] = {
	{ .data_offset = 41, .data_bits = 144, .qp = 21 },
	{ .data_offset = 67, .data_bits = 96, .qp = 24, .poc = 2,
	  .cabac_init_idc = 0, .predicted = true },
};

static const unsigned int h264_output[] = { POC_OUTPUT_0, POC_OUTPUT_1 };
static const unsigned int h264_pmv[] = { POC_PMV_0, POC_PMV_1 };

struct h264_poc {
	struct device *dev;
	void __iomem *vdh, *pxp, *global, *scd, *smmu;
	u8 *arena;
	__le32 *table;
	dma_addr_t arena_dma, table_dma;
	struct completion done;
	struct mutex mutex;
	int pxp_irq, smmu_irq;
	u32 status, fault, completions, fault_irqs;
	unsigned int frames_valid, y_bytes;
	bool failed_reset, probe_linear;
};

static u32 h264_addr(unsigned int offset)
{
	return (POC_IOVA + offset) >> 4;
}

static void h264_poc_messages(struct h264_poc *poc, unsigned int frame,
			      const struct h264_poc_picture *picture, bool cabac)
{
	unsigned int msg = frame * POC_MSG_STRIDE;
	unsigned int up = POC_UP_MSG + frame * POC_UP_MSG_SIZE;
	__le32 *pub = (__le32 *)(poc->arena + msg);
	__le32 *core = (__le32 *)(poc->arena + msg + POC_CORE);
	__le32 *pic = (__le32 *)(poc->arena + msg + POC_PICTURE);
	__le32 *slice = (__le32 *)(poc->arena + msg + POC_SLICE);
	unsigned int output = h264_output[frame];
	unsigned int reference = h264_output[0];
	unsigned int yuv_bytes = poc->y_bytes + poc->y_bytes / 2;
	u32 yuv_end = output + yuv_bytes;
	unsigned int i;

	memset(poc->arena + up, POC_POISON, POC_UP_MSG_SIZE);
	memset(poc->arena + output, POC_POISON, yuv_bytes);

	pub[2] = cpu_to_le32(0x41000003);
	pub[3] = cpu_to_le32(0x00030000 |
			     (poc->probe_linear ? POC_PUB_2D_REQUEST : 0));
	pub[4] = cpu_to_le32(h264_addr(msg + POC_PICTURE));
	pub[5] = cpu_to_le32(h264_addr(up));
	pub[6] = cpu_to_le32(h264_addr(POC_STREAM));
	pub[8] = cpu_to_le32(1);
	pub[9] = cpu_to_le32(h264_addr(output));
	pub[10] = cpu_to_le32(POC_STRIDE);
	pub[12] = cpu_to_le32(poc->y_bytes);
	pub[20] = cpu_to_le32(h264_addr(yuv_end));
	pub[22] = cpu_to_le32(h264_addr(POC_EXT_WORK + 0x6000));
	pub[23] = cpu_to_le32(h264_addr(h264_pmv[frame]));
	pub[24] = cpu_to_le32(h264_addr(POC_EXT_WORK + 0xe000));
	pub[25] = cpu_to_le32(h264_addr(POC_CABAC));
	pub[30] = cpu_to_le32(h264_addr(POC_EXT_WORK + 0x1e000));
	pub[31] = cpu_to_le32(0x00010001);

	/* POC_CORE (+0x100) follows pub[] implicitly; core[28/32/36] come from dec_hal_write_core_msg. */
	core[28] = cpu_to_le32(h264_addr(msg + POC_SLICE));
	core[32] = cpu_to_le32(h264_addr(up + 0x80));
	core[36] = cpu_to_le32(1);

	/* Progressive 4:2:0, with entropy_coding_mode_flag at bit 27. */
	pic[0] = cpu_to_le32(0x02010001 | (cabac ? BIT(27) : 0));
	pic[2] = cpu_to_le32(picture->poc);
	for (i = 64; i < 120; i++)
		pic[i] = cpu_to_le32(0x10101010);

	slice[0] = cpu_to_le32(picture->data_offset / 16);
	slice[1] = cpu_to_le32((picture->data_offset % 16) * 8 + picture->bit_offset);
	slice[2] = cpu_to_le32(picture->data_bits);
	slice[6] = cpu_to_le32((u32)picture->qp << 26 |
			      (u32)picture->cabac_init_idc << 24);
	slice[10] = cpu_to_le32((30U << 5) | 30U);
	slice[0xb0 / 4] = cpu_to_le32(3);
	slice[0xf8 / 4] = cpu_to_le32(0x00010001);

	if (picture->predicted) {
		pic[10] = cpu_to_le32(h264_addr(reference));
		/* DPB[0] top/bottom POC, both zero for the progressive IDR. */
		pic[27] = 0;
		pic[28] = 0;
		/* P slice, one active L0 entry, progressive DPB slot 0. */
		slice[7] = cpu_to_le32(1);
		slice[12] = 0;
		slice[20] = 0;
		/* ColMV reads in slice words 8/9 are used by B slices only. */
	}
	for (i = 0; i < 16; i++) {
		core[0x500 / 4 + i] = cpu_to_le32(h264_addr(reference));
		core[0x540 / 4 + i] = cpu_to_le32(h264_addr(reference + yuv_bytes));
		core[0x580 / 4 + i] = cpu_to_le32(h264_addr(reference));
	}
	for (i = 0; i < 3; i++) {
		core[0x5cc / 4 + i] = cpu_to_le32(0x00200020);
		core[0x5d8 / 4 + i] = cpu_to_le32(poc->y_bytes);
		core[0x5e4 / 4 + i] = 0;
		core[0x5fc / 4 + i] = cpu_to_le32(POC_STRIDE);
	}
	core[0x614 / 4] = cpu_to_le32(h264_addr(yuv_end));
	core[0x618 / 4] = 0;
	core[0x61c / 4] = cpu_to_le32(h264_addr(output));
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
		WRITE_ONCE(poc->completions, poc->completions + 1);
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
	WRITE_ONCE(poc->fault_irqs, poc->fault_irqs + 1);
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

static void h264_poc_dump_diag(struct h264_poc *poc, unsigned int frame)
{
	unsigned int up_offset = POC_UP_MSG + frame * POC_UP_MSG_SIZE;
	const u8 *up = poc->arena + up_offset;
	const u8 *wr = poc->arena + POC_FAULT_WRITE;
	const u8 *y = poc->arena + h264_output[frame];
	const u8 *uv = y + poc->y_bytes;
	bool wr_sink_nz = memchr_inv(wr, 0, 512);
	bool y_mod = memchr_inv(y, POC_POISON, poc->y_bytes);
	bool uv_mod = memchr_inv(uv, POC_POISON, poc->y_bytes / 2);
	bool up_tail_mod = memchr_inv(up + POC_UP_MSG_RESPONSE, POC_POISON,
				     POC_UP_MSG_SIZE - POC_UP_MSG_RESPONSE);
	unsigned int i, up_changed = 0;

	for (i = 0; i < POC_UP_MSG_RESPONSE; i++)
		up_changed += up[i] != POC_POISON;
	dev_info(poc->dev,
		 "SMMU+0x28=%08x wr_sink_nz=%d y_mod=%d uv_mod=%d up_iova=%08x up_changed=%u up_tail_mod=%d\n",
		 readl(poc->smmu + SMMU_IRQ_STATUS), wr_sink_nz, y_mod, uv_mod,
		 POC_IOVA + up_offset, up_changed, up_tail_mod);
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
	if (y_mod)
		print_hex_dump(KERN_INFO, "Y_RAW ", DUMP_PREFIX_OFFSET, 32, 1,
			       y, poc->y_bytes, false);
	if (uv_mod)
		print_hex_dump(KERN_INFO, "UV_RAW ", DUMP_PREFIX_OFFSET, 32, 1,
			       uv, poc->y_bytes / 2, false);
}

static int h264_poc_decode(struct h264_poc *poc, unsigned int frame,
			    const struct h264_poc_picture *picture, bool cabac)
{
	void __iomem *slot = poc->vdh + POC_SLOT + frame * POC_SLOT_STRIDE;
	const u8 *output = poc->arena + h264_output[frame];
	u32 completions = READ_ONCE(poc->completions);
	u32 fault_irqs = READ_ONCE(poc->fault_irqs);
	unsigned long timeout;
	u32 status, fault, done_count, fault_count;
	int ret = 0;

	h264_poc_messages(poc, frame, picture, cabac);
	WRITE_ONCE(poc->status, 0);
	WRITE_ONCE(poc->fault, 0);
	reinit_completion(&poc->done);

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
	writel(0x300, slot + 0x04);
	writel(h264_addr(frame * POC_MSG_STRIDE), slot + 0x08);
	dma_wmb();
	writel(1, slot);
	writel(1, slot + 0x10);

	timeout = wait_for_completion_timeout(&poc->done, msecs_to_jiffies(500));
	synchronize_irq(poc->pxp_irq);
	synchronize_irq(poc->smmu_irq);
	dma_rmb();
	status = READ_ONCE(poc->status);
	fault = READ_ONCE(poc->fault) | readl(poc->smmu + SMMU_IRQ_STATUS);
	done_count = READ_ONCE(poc->completions) - completions;
	fault_count = READ_ONCE(poc->fault_irqs) - fault_irqs;
	dev_info(poc->dev,
		 "frame %u %s completion=%08x PXP completions=%u SMMU faults=%08x SMMU IRQs=%u\n",
		 frame, picture->predicted ? "P" : "IDR", status, done_count,
		 fault, fault_count);
	if (fault || fault_count)
		ret = -EFAULT;
	else if (!timeout)
		ret = -ETIMEDOUT;
	else if (done_count != 1 || ((status >> 24) & 7) != frame ||
		 !(status & PXP_DECODE_DONE) ||
		 (status & (PXP_DECODE_ERROR | 0xffff)))
		ret = -EIO;
	else if (!memchr_inv(output, POC_POISON, poc->y_bytes) ||
		 !memchr_inv(output + poc->y_bytes, POC_POISON, poc->y_bytes / 2))
		ret = -ENODATA;
	if (ret)
		h264_poc_dump_diag(poc, frame);
	return ret;
}

static int h264_poc_run(struct h264_poc *poc, bool cabac, bool probe_linear)
{
	const struct h264_poc_picture *pictures = cabac ? h264_cabac_pictures : &h264_cavlc_idr;
	unsigned int frames = cabac ? ARRAY_SIZE(h264_cabac_pictures) : 1;
	const u8 *stream = cabac ? h264_cabac_stream : h264_test_stream;
	size_t stream_size = cabac ? sizeof(h264_cabac_stream) : sizeof(h264_test_stream);
	unsigned int frame;
	int ret = 0, reset_ret;

	if (poc->failed_reset)
		return -EIO;
	poc->frames_valid = 0;
	poc->probe_linear = probe_linear;
	poc->y_bytes = probe_linear ? POC_LINEAR_Y_BYTES : POC_TILED_Y_BYTES;
	reset_ret = hi3798mv320_vpu_reset(poc->dev, poc->global, poc->pxp);
	if (reset_ret)
		goto reset_failed;

	/* The same pinned IOVA mappings cover both pictures and their PMV. */
	memset(poc->arena, 0, POC_ARENA_SIZE);
	memcpy(poc->arena + POC_STREAM, stream, stream_size);
	memcpy(poc->arena + POC_CABAC, h264_cabac_init, sizeof(h264_cabac_init));
	dma_wmb();
	writel(lower_32_bits(poc->table_dma), poc->smmu + SMMU_TABLE);
	writel(lower_32_bits(poc->arena_dma + POC_FAULT_READ), poc->smmu + SMMU_READ_SINK);
	writel(lower_32_bits(poc->arena_dma + POC_FAULT_WRITE), poc->smmu + SMMU_WRITE_SINK);
	writel(8, poc->smmu + SMMU_CTRL);
	writel(0xf, poc->smmu + SMMU_IRQ_CLEAR);
	writel(0, poc->smmu + SMMU_IRQ_MASK);

	dev_info(poc->dev, "%s frames=%u 2d_request=%d stride=%u y_bytes=%u\n",
		 cabac ? "CABAC" : "CAVLC", frames, probe_linear,
		 POC_STRIDE, poc->y_bytes);
	/* VDH consumes consecutive hardware slots under the same SMMU mapping. */
	for (frame = 0; frame < frames; frame++) {
		ret = h264_poc_decode(poc, frame, &pictures[frame], cabac);
		if (ret)
			break;
		poc->frames_valid++;
	}

	reset_ret = hi3798mv320_vpu_reset(poc->dev, poc->global, poc->pxp);
	if (reset_ret)
		goto reset_failed;
	writel(0xf, poc->smmu + SMMU_IRQ_MASK);
	synchronize_irq(poc->pxp_irq);
	synchronize_irq(poc->smmu_irq);
	if (h264_poc_fault(poc) | READ_ONCE(poc->fault)) {
		poc->frames_valid = 0;
		return -EFAULT;
	}
	return ret;

reset_failed:
	poc->frames_valid = 0;
	poc->failed_reset = true;
	dev_crit(poc->dev, "PXP reset failed, DMA pinned until reboot\n");
	return reset_ret;
}

static ssize_t decode_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct h264_poc *poc = dev_get_drvdata(dev);
	bool cabac = sysfs_streq(buf, "1");
	bool probe_linear = sysfs_streq(buf, "probe-linear");
	int ret;

	if (!cabac && !probe_linear && !sysfs_streq(buf, "cavlc"))
		return -EINVAL;

	mutex_lock(&poc->mutex);
	ret = h264_poc_run(poc, cabac, probe_linear);
	mutex_unlock(&poc->mutex);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(decode);

static ssize_t h264_frame_show(struct device *dev, char *buf, unsigned int frame,
			       bool tiled)
{
	struct h264_poc *poc = dev_get_drvdata(dev);
	const u8 *y_plane = poc->arena + h264_output[frame];
	const u8 *vu_plane;
	char *pos = buf;
	unsigned int row, col, component, offset;

	mutex_lock(&poc->mutex);
	if (frame >= poc->frames_valid) {
		mutex_unlock(&poc->mutex);
		return -ENODATA;
	}
	vu_plane = y_plane + poc->y_bytes;
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
	return h264_frame_show(dev, buf, 0, true);
}
static DEVICE_ATTR_RO(frame);

static ssize_t frame1_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return h264_frame_show(dev, buf, 1, true);
}
static DEVICE_ATTR_RO(frame1);

static ssize_t linear_frame_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return h264_frame_show(dev, buf, 0, false);
}
static DEVICE_ATTR_RO(linear_frame);

static struct attribute *h264_poc_attrs[] = {
	&dev_attr_decode.attr,
	&dev_attr_frame.attr,
	&dev_attr_frame1.attr,
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
	dev_info(dev, "fixed H.264 CABAC IDR/P and CAVLC IDR ready, table=%pad arena=%pad IOVA=%08x\n",
		 &poc->table_dma, &poc->arena_dma, POC_IOVA);
	return 0;
}
