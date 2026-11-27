// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <media/media-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-vmalloc.h>

#include <asm/simd.h>

#include "hi3798mv320-vdec-cabac.h"
#include "hi3798mv320-vdh.h"
#include "hi3798mv320-tiled.h"

#define VDEC_NAME		"hi3798mv320-vdec"
#define VDEC_MIN_SIZE		32
#define VDEC_MAX_WIDTH		4096
#define VDEC_MAX_HEIGHT		2176
#define VDEC_MAX_STREAM		SZ_16M
#define VDEC_STREAM_PAD		256
#define VDEC_PAGE		SZ_4K
#define VDEC_TABLE_SIZE		SZ_4M
#define VDEC_IOVA_START		0x10000000U
#define VDEC_IOVA_SIZE		SZ_1G

#define VDH_SLOT		0x100
#define VDH_SLOT_STRIDE		0x20
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

/* Offsets within the coherent task workspace. */
#define MSG_STRIDE		0x2000
#define MSG_CORE		0x100
#define MSG_PICTURE		0x800
#define MSG_SLICE		0xe00
#define MSG_UP			0x32e00
#define MSG_UP_SIZE		0x400
#define CABAC_TABLE		0x52000
#define EXT_WORK		(CABAC_TABLE + 0x1800)
#define H264_EXT_WORK_SIZE	0x21000
#define SLICE_POOL		0x80000
#define SLICE_MSG_SIZE		0x400
#define VDEC_MAX_SLICES		136
#define STREAM_OFFSET		ALIGN(SLICE_POOL + (VDEC_MAX_SLICES - 1) * SLICE_MSG_SIZE, VDEC_PAGE)

#define HEVC_SLICE_UP		(MSG_UP + 0x80)
#define HEVC_TILE_INFO		0x33c00
#define HEVC_TILE_MAP_SIZE	512
#define HEVC_EXT_WORK		(CABAC_TABLE + 0x400)
#define HEVC_EXT_WORK_SIZE	0x38100
#define HEVC_STREAM_OFFSET	ALIGN(HEVC_EXT_WORK + HEVC_EXT_WORK_SIZE, VDEC_PAGE)
#define HEVC_MAX_SLICES		200
#define HEVC_MAX_TILE_COLUMNS	20
#define HEVC_MAX_TILE_ROWS	22
#define HEVC_TILE_INFO_SIZE	(2 * HEVC_TILE_MAP_SIZE + 4 * (HEVC_MAX_TILE_COLUMNS + HEVC_MAX_TILE_ROWS))

static_assert(MSG_SLICE + HEVC_MAX_SLICES * SLICE_MSG_SIZE <= MSG_UP);
static_assert(HEVC_TILE_INFO + HEVC_TILE_INFO_SIZE <= CABAC_TABLE);
static_assert(sizeof(hevc_cabac_init) <= HEVC_EXT_WORK - CABAC_TABLE);
static_assert(EXT_WORK + H264_EXT_WORK_SIZE <= SLICE_POOL);
static_assert(VDEC_MAX_WIDTH / 16 <= HEVC_TILE_MAP_SIZE);
static_assert(VDEC_MAX_HEIGHT / 16 <= HEVC_TILE_MAP_SIZE);

struct vdec_dma {
	void *cpu;
	dma_addr_t dma;
	u32 iova;
	size_t size;
	bool cached;
	bool device_owned;
};

struct vdec_buffer {
	struct v4l2_m2m_buffer m2m;
	struct vdec_dma surface;
	struct dma_buf *native_pin;
	u64 timestamp;
	bool decoded;
};

struct vdec_refs {
	struct vdec_buffer *buffers[V4L2_H264_NUM_DPB_ENTRIES];
	u8 index[V4L2_H264_NUM_DPB_ENTRIES];
};

struct vdec_dev {
	struct device *dev;
	void __iomem *vdh, *pxp, *global, *scd, *smmu;
	struct completion done;
	struct work_struct work;
	struct mutex queue_lock;
	struct mutex hw_lock;
	struct gen_pool *iova_pool;
	__le32 *table;
	dma_addr_t table_dma;
	void *sinks;
	dma_addr_t sinks_dma;
	int pxp_irq, smmu_irq;
	ktime_t pxp_done_at;
	u32 status, fault;
	unsigned int slot;
	bool tlb_dirty, dead;

	struct media_device media;
	struct v4l2_device v4l2;
	struct video_device video;
	struct v4l2_m2m_dev *m2m;
};

struct vdec_ctx;

struct vdec_codec_ops {
	u32 pixelformat;
	u32 slice_control;
	void (*surface_layout)(struct vdec_ctx *ctx);
	int (*prepare_workspace)(struct vdec_ctx *ctx);
	int (*pack_request)(struct vdec_ctx *ctx, struct vb2_v4l2_buffer *src,
			    struct vb2_v4l2_buffer *dst, unsigned int controls);
	void (*end_picture)(struct vdec_ctx *ctx);
	unsigned int (*request_controls)(struct v4l2_ctrl_handler *hdl);
};

struct vdec_hevc_tiles {
	u16 col[HEVC_MAX_TILE_COLUMNS + 1];
	u16 row[HEVC_MAX_TILE_ROWS + 1];
	unsigned int cols, rows;
};

struct vdec_timing {
	u64 rbsp_ns, hw_ns, detile_ns, clear_ns, sync_ns;
	u32 frames;
};

struct vdec_ctx {
	struct vdec_dev *vdec;
	const struct vdec_codec_ops *codec;
	struct v4l2_fh fh;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *sps, *pps, *scaling, *slice, *decode, *weights;
	struct v4l2_ctrl *hevc_sps, *hevc_pps, *hevc_scaling, *hevc_slice, *hevc_decode;
	struct v4l2_pix_format_mplane output, capture;
	struct vdec_dma workspace;
	unsigned int stride, y_size, vu_size, tail_size, pmv_offset, pmv_size;
	unsigned int sequence_out, sequence_cap;
	unsigned int num_slices, stream_used, last_mb;
	u64 picture_rbsp_ns, picture_sync_ns;
	struct vdec_timing timing;
	struct vdec_buffer *held;
	u64 held_timestamp;
	struct v4l2_ctrl_h264_sps picture_sps;
	struct v4l2_ctrl_h264_pps picture_pps;
	struct v4l2_ctrl_h264_decode_params picture_dec;
	struct v4l2_ctrl_hevc_sps hevc_picture_sps;
	struct v4l2_ctrl_hevc_pps hevc_picture_pps;
	struct v4l2_ctrl_hevc_decode_params hevc_picture_dec;
	struct vdec_hevc_tiles hevc_tiles;
};

static void vdec_set_capture(struct vdec_ctx *ctx, u32 fourcc);

static struct vdec_ctx *file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct vdec_ctx, fh);
}

static struct vdec_buffer *to_vdec_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct vdec_buffer, m2m.vb);
}

static int vdec_reset(struct vdec_dev *vdec);

static bool vdec_is_tiled(u32 fourcc)
{
	return fourcc == MV320_TILED_8 || fourcc == MV320_TILED_10;
}

static void vdec_map_pages(struct vdec_dev *vdec, u32 iova, dma_addr_t dma, size_t size)
{
	unsigned int pte = iova / VDEC_PAGE, i;

	for (i = 0; i < size / VDEC_PAGE; i++)
		vdec->table[pte + i] = cpu_to_le32((dma + i * VDEC_PAGE) | 1);
}

static void vdec_dma_release(struct vdec_dev *vdec, struct vdec_dma *buf)
{
	if (buf->cached)
		dma_free_noncoherent(vdec->dev, buf->size, buf->cpu, buf->dma,
				     DMA_BIDIRECTIONAL);
	else
		dma_free_coherent(vdec->dev, buf->size, buf->cpu, buf->dma);
	buf->cpu = NULL;
}

/* The flat page table is private to the decoder and uses 4 KiB PTEs. */
static int vdec_dma_alloc(struct vdec_dev *vdec, struct vdec_dma *buf, size_t size,
			  bool cached)
{
	lockdep_assert_held(&vdec->hw_lock);
	if (vdec->dead)
		return -EIO;

	buf->size = ALIGN(size, VDEC_PAGE);
	buf->iova = gen_pool_alloc(vdec->iova_pool, buf->size);
	if (!buf->iova)
		return -ENOMEM;

	buf->cached = cached;
	buf->device_owned = false;
	if (cached)
		buf->cpu = dma_alloc_noncoherent(vdec->dev, buf->size, &buf->dma,
						DMA_BIDIRECTIONAL, GFP_KERNEL);
	else
		buf->cpu = dma_alloc_coherent(vdec->dev, buf->size, &buf->dma, GFP_KERNEL);
	if (!buf->cpu) {
		gen_pool_free(vdec->iova_pool, buf->iova, buf->size);
		return -ENOMEM;
	}
	if (!IS_ALIGNED(buf->dma, VDEC_PAGE)) {
		vdec_dma_release(vdec, buf);
		gen_pool_free(vdec->iova_pool, buf->iova, buf->size);
		return -EFAULT;
	}

	vdec_map_pages(vdec, buf->iova, buf->dma, buf->size);
	dma_wmb();
	dev_dbg(vdec->dev, "map IOVA %#x DMA %pad size %zu\n",
		buf->iova, &buf->dma, buf->size);
	return 0;
}

static bool vdec_unmap(struct vdec_dev *vdec, u32 iova, size_t size)
{
	lockdep_assert_held(&vdec->hw_lock);
	if (vdec->dead)
		return false;

	memset(vdec->table + iova / VDEC_PAGE, 0,
	       size / VDEC_PAGE * sizeof(*vdec->table));
	dma_wmb();

	/* Quiesce the walker and reload TTBR before handing out the IOVA again. */
	if (vdec->tlb_dirty && vdec_reset(vdec))
		return false;
	gen_pool_free(vdec->iova_pool, iova, size);
	return true;
}

static void vdec_dma_free(struct vdec_dev *vdec, struct vdec_dma *buf)
{
	if (!buf->cpu || !vdec_unmap(vdec, buf->iova, buf->size))
		return;
	vdec_dma_release(vdec, buf);
}

static void vdec_surface_sync(struct vdec_ctx *ctx, struct vdec_dma *surface,
			      bool for_device)
{
	ktime_t start;

	if (!surface->cached || surface->device_owned == for_device)
		return;

	start = ktime_get();
	if (for_device)
		dma_sync_single_for_device(ctx->vdec->dev, surface->dma, surface->size,
					   DMA_BIDIRECTIONAL);
	else
		dma_sync_single_for_cpu(ctx->vdec->dev, surface->dma, surface->size,
					DMA_BIDIRECTIONAL);
	surface->device_owned = for_device;
	ctx->picture_sync_ns += ktime_sub(ktime_get(), start);
}

static __le32 vdec_addr(u32 base, unsigned int offset)
{
	return cpu_to_le32((base + offset) >> 4);
}

static u32 vdec_pxp_ack(u32 status)
{
	return (status & PXP_DECODE_DONE ? BIT(0) : 0) |
	       (status & PXP_AUX_DONE ? BIT(2) : 0);
}

static irqreturn_t vdec_pxp_irq(int irq, void *data)
{
	struct vdec_dev *vdec = data;
	u32 status = readl(vdec->pxp + VDH_IRQ_STATUS);
	u32 ack = vdec_pxp_ack(status);

	if (!ack && !(status & PXP_DECODE_ERROR))
		return IRQ_NONE;
	if (status & PXP_DECODE_DONE)
		vdec->pxp_done_at = ktime_get();
	if (ack)
		writel(ack, vdec->pxp + PXP_IRQ_CLEAR);
	if (status & (PXP_DECODE_DONE | PXP_DECODE_ERROR)) {
		WRITE_ONCE(vdec->status, status);
		complete(&vdec->done);
	}
	return IRQ_HANDLED;
}

static irqreturn_t vdec_smmu_irq(int irq, void *data)
{
	struct vdec_dev *vdec = data;
	u32 fault = readl(vdec->smmu + SMMU_IRQ_STATUS);

	if (!fault)
		return IRQ_NONE;

	dev_err_ratelimited(vdec->dev, "SMMU fault %#x, read %#x write %#x\n",
			    fault, readl(vdec->smmu + SMMU_READ_FAULT),
			    readl(vdec->smmu + SMMU_WRITE_FAULT));
	writel(0xf, vdec->smmu + SMMU_IRQ_CLEAR);
	WRITE_ONCE(vdec->fault, fault);
	complete(&vdec->done);
	return IRQ_HANDLED;
}

static void vdec_configure_smmu(struct vdec_dev *vdec)
{
	dma_wmb();
	writel(vdec->table_dma, vdec->smmu + SMMU_TABLE);
	writel(vdec->sinks_dma, vdec->smmu + SMMU_READ_SINK);
	writel(vdec->sinks_dma + 512, vdec->smmu + SMMU_WRITE_SINK);
	writel(8, vdec->smmu + SMMU_CTRL);
	writel(0xf, vdec->smmu + SMMU_IRQ_CLEAR);
	writel(0, vdec->smmu + SMMU_IRQ_MASK);
}

static int vdec_reset(struct vdec_dev *vdec)
{
	int ret;

	if (vdec->dead)
		return -EIO;

	writel(0xf, vdec->smmu + SMMU_IRQ_MASK);
	disable_irq(vdec->pxp_irq);
	synchronize_irq(vdec->smmu_irq);
	ret = hi3798mv320_vpu_reset(vdec->dev, vdec->global, vdec->pxp);
	if (ret) {
		/* Keep potentially active DMA and its mappings pinned. */
		vdec->dead = true;
		__module_get(THIS_MODULE);
		dev_crit(vdec->dev, "decoder DMA pinned after failed reset\n");
		return ret;
	}

	vdec->slot = 0;
	vdec->tlb_dirty = false;
	writel(vdec_pxp_ack(readl(vdec->pxp + VDH_IRQ_STATUS)),
	       vdec->pxp + PXP_IRQ_CLEAR);
	vdec_configure_smmu(vdec);
	enable_irq(vdec->pxp_irq);
	return 0;
}

static unsigned int vdec_slice_offset(unsigned int index)
{
	return index ? SLICE_POOL + (index - 1) * SLICE_MSG_SIZE : MSG_SLICE;
}

static void vdec_h264_dump_task(struct vdec_ctx *ctx)
{
	static DEFINE_RATELIMIT_STATE(dump_limit, DEFAULT_RATELIMIT_INTERVAL, 1);
	const void *work = ctx->workspace.cpu;
	const __le32 *pub = work, *pic = work + MSG_PICTURE;
	unsigned int i;

	if (!__ratelimit(&dump_limit))
		return;

	dev_err(ctx->vdec->dev, "H.264 task: pic[0..2]=%08x/%08x/%08x pub[3,23]=%08x/%08x slices=%u\n",
		le32_to_cpu(pic[0]), le32_to_cpu(pic[1]), le32_to_cpu(pic[2]),
		le32_to_cpu(pub[3]), le32_to_cpu(pub[23]), ctx->num_slices);
	print_hex_dump(KERN_ERR, "H.264 task: UP[0..7] ", DUMP_PREFIX_OFFSET,
		       32, 4, work + MSG_UP, 32, false);
	print_hex_dump(KERN_ERR, "H.264 task: UP[32..39] ", DUMP_PREFIX_OFFSET,
		       32, 4, work + HEVC_SLICE_UP, 32, false);
	print_hex_dump(KERN_ERR, "H.264 task: pic[10..25] ", DUMP_PREFIX_OFFSET,
		       32, 4, pic + 10, 16 * sizeof(*pic), false);
	print_hex_dump(KERN_ERR, "H.264 task: pic[27..58] ", DUMP_PREFIX_OFFSET,
		       32, 4, pic + 27, 32 * sizeof(*pic), false);
	for (i = 0; i < min(ctx->num_slices, 4U); i++) {
		const __le32 *slice = work + vdec_slice_offset(i);

		dev_err(ctx->vdec->dev, "H.264 task: slice %u\n", i);
		print_hex_dump(KERN_ERR, "H.264 task: slice[0..43] ", DUMP_PREFIX_OFFSET,
			       32, 4, slice, 44 * sizeof(*slice), false);
	}
}

static int vdec_submit(struct vdec_ctx *ctx, u64 *hw_ns)
{
	struct vdec_dev *vdec = ctx->vdec;
	const struct vdec_dma *work = &ctx->workspace;
	void __iomem *slot = vdec->vdh + VDH_SLOT + vdec->slot * VDH_SLOT_STRIDE;
	unsigned long done;
	ktime_t submitted_at;
	u32 status, fault;
	int ret = 0;

	WRITE_ONCE(vdec->status, 0);
	WRITE_ONCE(vdec->fault, 0);
	reinit_completion(&vdec->done);

	writel(0xaaaaaaaa, vdec->vdh + 0x00);
	writel(0xaaaaaaaa, vdec->vdh + 0x04);
	writel(0x000b0003, vdec->vdh + 0x08);
	writel(0x40, vdec->vdh + 0x14);
	writel(1, vdec->global + 0x08);
	writel(vdec_pxp_ack(readl(vdec->pxp + VDH_IRQ_STATUS)),
	       vdec->pxp + PXP_IRQ_CLEAR);
	writel(1, vdec->pxp + PXP_BSP_CLEAR);
	writel(1, vdec->pxp + VDH_IRQ_MASK);
	writel(0, vdec->pxp + VDH_IRQ_AUX_MASK);
	writel(0x300, slot + 0x04);
	writel(work->iova >> 4, slot + 0x08);
	dma_wmb();
	vdec->tlb_dirty = true;
	submitted_at = ktime_get();
	writel(1, slot);
	writel(1, slot + 0x10);

	done = wait_for_completion_timeout(&vdec->done, msecs_to_jiffies(500));
	synchronize_irq(vdec->pxp_irq);
	synchronize_irq(vdec->smmu_irq);
	dma_rmb();
	status = READ_ONCE(vdec->status);
	fault = READ_ONCE(vdec->fault) | readl(vdec->smmu + SMMU_IRQ_STATUS);
	if (fault)
		ret = -EFAULT;
	else if (!done)
		ret = -ETIMEDOUT;
	else if (FIELD_GET(GENMASK(26, 24), status) != vdec->slot ||
		 !(status & PXP_DECODE_DONE) || (status & (PXP_DECODE_ERROR | 0xffff)))
		ret = -EIO;

	if (!ret)
		*hw_ns = ktime_sub(vdec->pxp_done_at, submitted_at);

	if (ret) {
		const __le32 *up = work->cpu + MSG_UP;

		dev_err_ratelimited(vdec->dev,
				    "slot %u: decode %d PXP %#x SMMU %#x regs %08x/%08x/%08x UP %08x/%08x/%08x\n",
				    vdec->slot, ret, status, fault, readl(vdec->pxp + 0x20),
				    readl(vdec->pxp + 0x24), readl(vdec->pxp + 0x28),
				    le32_to_cpu(up[0]), le32_to_cpu(up[2]),
				    le32_to_cpu(up[0x80 / 4]));
		vdec_reset(vdec);
		if (!vdec->dead && ctx->output.pixelformat == V4L2_PIX_FMT_H264_SLICE)
			vdec_h264_dump_task(ctx);
	} else if (vdec->slot == 1) {
		/* The hardware cursor advances past our two slots. Rewind it. */
		ret = vdec_reset(vdec);
	} else {
		vdec->slot = 1;
	}
	return ret;
}

static struct vdec_buffer *vdec_find_buffer(struct vdec_ctx *ctx,
					   const struct vdec_buffer *dst, u64 timestamp)
{
	struct vb2_queue *vq = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	unsigned int i;

	for (i = 0; i < vq->max_num_buffers; i++) {
		struct vb2_buffer *vb = vb2_get_buffer(vq, i);
		struct vdec_buffer *ref;

		if (!vb)
			continue;
		ref = to_vdec_buffer(vb);
		if (ref != dst && ref->decoded && ref->timestamp == timestamp) {
			/* The CPU has finished reading the tiled surface. */
			vdec_surface_sync(ctx, &ref->surface, true);
			return ref;
		}
	}
	return NULL;
}

static int vdec_h264_find_refs(struct vdec_ctx *ctx, struct vdec_buffer *dst,
			  const struct v4l2_ctrl_h264_decode_params *dec,
			  struct vdec_refs *refs)
{
	unsigned int i, index = 0;

	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		if (!(dec->dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			continue;
		if (!(dec->dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID) ||
		    dec->dpb[i].fields != V4L2_H264_FRAME_REF)
			return -EINVAL;

		refs->buffers[i] = vdec_find_buffer(ctx, dst, dec->dpb[i].reference_ts);
		if (!refs->buffers[i])
			return -ENOENT;
		refs->index[i] = index++;
	}
	return 0;
}

static void vdec_scaling(__le32 *pic,
			 const struct v4l2_ctrl_h264_scaling_matrix *scaling)
{
	unsigned int i, j;

	for (i = 0; i < 56; i++)
		pic[64 + i] = cpu_to_le32(0x10101010);
	if (!scaling)
		return;

	for (i = 0; i < 6; i++)
		for (j = 0; j < 4; j++) {
			const u8 *v = &scaling->scaling_list_4x4[i][j * 4];

			pic[64 + i * 4 + j] = cpu_to_le32(v[0] | v[1] << 8 |
							 v[3] << 16 | (u32)v[2] << 24);
		}

	for (i = 0; i < 2; i++)
		for (j = 0; j < 8; j++) {
			const u8 *v = &scaling->scaling_list_8x8[i][j * 8];

			pic[88 + i * 16 + j * 2] = cpu_to_le32(v[0] | v[2] << 8 |
							       v[4] << 16 | (u32)v[6] << 24);
			pic[89 + i * 16 + j * 2] = cpu_to_le32(v[1] | v[3] << 8 |
							       v[5] << 16 | (u32)v[7] << 24);
		}
}

static int vdec_ref_list(__le32 *slice, unsigned int list,
			 const struct v4l2_h264_reference *entries, unsigned int count,
			 const struct vdec_refs *refs,
			 const struct v4l2_ctrl_h264_decode_params *dec)
{
	unsigned int i, idx, slot;
	u32 mapped = 0;

	if (count > V4L2_H264_NUM_DPB_ENTRIES)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		idx = entries[i].index;
		if (idx >= V4L2_H264_NUM_DPB_ENTRIES || !refs->buffers[idx] ||
		    entries[i].fields != V4L2_H264_FRAME_REF)
			return -EINVAL;

		if (dec->dpb[idx].flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM)
			slice[12 + 4 * list + i / 8] |= cpu_to_le32(8U << (4 * (i % 8)));
		slot = refs->index[idx];
		slice[20 + 8 * list + i / 4] |= cpu_to_le32((2 * slot) << (5 * (i % 4)));
		if (!list && !(mapped & BIT(slot))) {
			/* Map both frame entries back to the first matching L0 index. */
			slice[36 + slot / 2] |= cpu_to_le32((i | i << 5) << (10 * (slot % 2)));
			mapped |= BIT(slot);
		}
	}
	return 0;
}

static void vdec_pred_weights(__le32 *slice, unsigned int list, unsigned int count,
			      const struct v4l2_ctrl_h264_pred_weights *weights)
{
	const struct v4l2_h264_weight_factors *factors = &weights->weight_factors[list];
	unsigned int i, base = 0x100 / 4 + list * (0x80 / 4);
	unsigned int y_denom = weights->luma_log2_weight_denom;
	unsigned int c_denom = weights->chroma_log2_weight_denom;

	for (i = 0; i < count; i++) {
		slice[base + i] = cpu_to_le32(y_denom | (factors->luma_weight[i] & 0x1ff) << 3 |
					     (u32)(u8)factors->luma_offset[i] << 12);
		slice[base + 0x100 / 4 + i] =
			cpu_to_le32(c_denom | (factors->chroma_weight[i][0] & 0x1ff) << 3 |
				    (u32)(u8)factors->chroma_offset[i][0] << 12);
		slice[base + 0x200 / 4 + i] =
			cpu_to_le32((factors->chroma_weight[i][1] & 0x1ff) |
				    (u32)(u8)factors->chroma_offset[i][1] << 9 | c_denom << 17);
	}
}

static void vdec_slice_end(__le32 *slice, unsigned int last, unsigned int width)
{
	slice[0xb0 / 4] = cpu_to_le32(last);
	slice[0xf8 / 4] = cpu_to_le32(last / width << 16 | (last % width));
}

static int vdec_slice_message(struct vdec_ctx *ctx, __le32 *slice,
			      const struct vdec_refs *refs, unsigned int data_bit,
			      unsigned int data_bits, bool has_weights)
{
	const struct v4l2_ctrl_h264_slice_params *sp = ctx->slice->p_cur.p_h264_slice_params;
	const struct v4l2_ctrl_h264_sps *sps = &ctx->picture_sps;
	const struct v4l2_ctrl_h264_decode_params *dec = &ctx->picture_dec;
	const struct v4l2_ctrl_h264_pps *pps = &ctx->picture_pps;
	unsigned int type, l0 = 0, l1 = 0, weighted = 0;
	unsigned int width = ctx->output.width / 16;
	int qp = 26 + pps->pic_init_qp_minus26 + sp->slice_qp_delta;
	int ret;

	switch (sp->slice_type) {
	case V4L2_H264_SLICE_TYPE_I:
		type = 0;
		break;
	case V4L2_H264_SLICE_TYPE_P:
		type = 1;
		l0 = sp->num_ref_idx_l0_active_minus1 + 1;
		weighted = !!(pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED);
		break;
	case V4L2_H264_SLICE_TYPE_B:
		type = 2;
		l0 = sp->num_ref_idx_l0_active_minus1 + 1;
		l1 = sp->num_ref_idx_l1_active_minus1 + 1;
		weighted = pps->weighted_bipred_idc;
		break;
	default:
		return -EINVAL;
	}
	if (qp < 0 || qp > 51 || sp->redundant_pic_cnt || sp->colour_plane_id ||
	    sp->cabac_init_idc > 2 || sp->disable_deblocking_filter_idc > 2)
		return -EINVAL;

	ret = vdec_ref_list(slice, 0, sp->ref_pic_list0, l0, refs, dec);
	if (ret)
		return ret;
	ret = vdec_ref_list(slice, 1, sp->ref_pic_list1, l1, refs, dec);
	if (ret)
		return ret;

	slice[0] = cpu_to_le32(data_bit / 128);
	slice[1] = cpu_to_le32(data_bit % 128);
	slice[2] = cpu_to_le32(data_bits);
	slice[6] = cpu_to_le32((u32)qp << 26 | (u32)sp->cabac_init_idc << 24 |
			      sp->first_mb_in_slice);
	/* Configure the PMV layout on producers as well as ColMV readers. */
	slice[7] = cpu_to_le32(type | l0 << 2 | l1 << 8 | (l0 ? (l0 - 1) << 16 : 0) |
			      (l1 ? (l1 - 1) << 21 : 0) |
			      (sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE ? BIT(14) : 0) |
			      (sp->flags & V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED ?
			       BIT(15) : 0));
	if (l1) {
		struct vdec_buffer *col = refs->buffers[sp->ref_pic_list1[0].index];

		slice[8] = vdec_addr(col->surface.iova, ctx->pmv_offset);
	}
	slice[10] = cpu_to_le32(((pps->chroma_qp_index_offset & 31) << 5) |
			       (pps->second_chroma_qp_index_offset & 31) |
			       (weighted << 16));
	slice[11] = cpu_to_le32(sp->disable_deblocking_filter_idc |
			       ((sp->slice_alpha_c0_offset_div2 & 15) << 8) |
			       ((sp->slice_beta_offset_div2 & 15) << 16));
	slice[0xf4 / 4] = cpu_to_le32(sp->first_mb_in_slice / width << 16 |
				      (sp->first_mb_in_slice % width));
	if (V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(pps, sp)) {
		const struct v4l2_ctrl_h264_pred_weights *weights =
			ctx->weights->p_cur.p_h264_pred_weights;

		if (!has_weights)
			return -ENOENT;
		vdec_pred_weights(slice, 0, l0, weights);
		vdec_pred_weights(slice, 1, l1, weights);
	}
	return 0;
}

static void vdec_picture_message(struct vdec_ctx *ctx, struct vdec_buffer *dst,
				 const struct vdec_refs *refs)
{
	const struct v4l2_ctrl_h264_pps *pps = &ctx->picture_pps;
	const struct v4l2_ctrl_h264_decode_params *dec = &ctx->picture_dec;
	u32 base = ctx->workspace.iova, out = dst->surface.iova;
	void *work = ctx->workspace.cpu;
	__le32 *pub = work, *core = work + MSG_CORE, *pic = work + MSG_PICTURE;
	unsigned int mb_width = ctx->output.width / 16, mb_height = ctx->output.height / 16;
	unsigned int surface_size = ctx->y_size + ctx->vu_size;
	unsigned int mbs = mb_width * mb_height, i;
	u32 geometry = (mb_height - 1) << 16 | (mb_width - 1);
	u32 fallback = out;

	memset(work, 0, MSG_STRIDE);
	memset(work + MSG_UP, 0, MSG_UP_SIZE);
	pub[2] = cpu_to_le32(0x41000000 | (mbs - 1));
	pub[3] = cpu_to_le32(0x30000 | (dec->nal_ref_idc ? BIT(15) : 0));
	pub[4] = vdec_addr(base, MSG_PICTURE);
	pub[5] = vdec_addr(base, MSG_UP);
	pub[6] = vdec_addr(base, STREAM_OFFSET);
	pub[8] = cpu_to_le32(1);
	pub[9] = vdec_addr(out, 0);
	pub[10] = cpu_to_le32(ctx->stride);
	pub[12] = cpu_to_le32(ctx->y_size);
	pub[20] = vdec_addr(out, surface_size);
	pub[22] = vdec_addr(base, EXT_WORK + 0x6000);
	pub[23] = vdec_addr(out, ctx->pmv_offset);
	pub[24] = vdec_addr(base, EXT_WORK + 0xe000);
	pub[25] = vdec_addr(base, CABAC_TABLE);
	pub[30] = vdec_addr(base, EXT_WORK + 0x1e000);
	pub[31] = cpu_to_le32(geometry);

	core[28] = vdec_addr(base, MSG_SLICE);
	core[32] = vdec_addr(base, HEVC_SLICE_UP);
	core[36] = cpu_to_le32(1);

	pic[0] = cpu_to_le32(geometry | BIT(25) | (dec->nal_ref_idc ? BIT(31) : 0) |
			     (pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE ? BIT(27) : 0) |
			     (pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED ? BIT(26) : 0) |
			     (pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE ? BIT(28) : 0));
	pic[1] = cpu_to_le32((pps->weighted_bipred_idc << 1) |
			     !!(pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED));
	pic[2] = cpu_to_le32(min(dec->top_field_order_cnt, dec->bottom_field_order_cnt));
	vdec_scaling(pic, pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT ?
		     ctx->scaling->p_cur.p_h264_scaling_matrix : NULL);

	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const struct vdec_buffer *buffer = refs->buffers[i];
		unsigned int slot = refs->index[i];
		u32 ref;

		if (!buffer)
			continue;
		ref = buffer->surface.iova;
		if (!slot)
			fallback = ref;
		pic[10 + slot] = vdec_addr(ref, 0);
		pic[27 + 2 * slot] = cpu_to_le32(dec->dpb[i].top_field_order_cnt);
		pic[28 + 2 * slot] = cpu_to_le32(dec->dpb[i].bottom_field_order_cnt);
		core[0x500 / 4 + slot] = vdec_addr(ref, 0);
		core[0x540 / 4 + slot] = vdec_addr(ref, surface_size);
		core[0x580 / 4 + slot] = vdec_addr(ref, 0);
	}
	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		if (core[0x500 / 4 + i])
			continue;
		core[0x500 / 4 + i] = vdec_addr(fallback, 0);
		core[0x540 / 4 + i] = vdec_addr(fallback, surface_size);
		core[0x580 / 4 + i] = vdec_addr(fallback, 0);
	}

	for (i = 0; i < 3; i++) {
		core[0x5cc / 4 + i] = cpu_to_le32(ctx->output.width | ctx->output.height << 16);
		core[0x5d8 / 4 + i] = cpu_to_le32(ctx->y_size);
		core[0x5fc / 4 + i] = cpu_to_le32(ctx->stride);
	}
	core[0x614 / 4] = vdec_addr(out, surface_size);
	core[0x61c / 4] = vdec_addr(out, 0);
}

/* VDH rotates the row within every second/next horizontal tile. */
static void vdec_detile_plane(u8 *dst, const u8 *src, unsigned int width,
			      unsigned int height, unsigned int stride,
			      unsigned int tile_height)
{
	unsigned int band_row, col;
	unsigned int mask = tile_height / 4 - 1;

	for (band_row = 0; band_row < height; band_row += tile_height) {
		const u8 *tile = src + band_row * stride;
		u8 *band = dst + band_row * width;
		unsigned int rows = min(tile_height, height - band_row);

		for (col = 0; col < width; col += 64, tile += 64 * tile_height) {
			unsigned int swizzle = (col / 64 & mask) * 4;
			unsigned int row;

			for (row = 0; row < rows; row++) {
				const u8 *in = tile + (row ^ swizzle) * 64;
				u8 *out = band + row * width + col;

				if (col + 64 <= width)
					memcpy(out, in, 64);
				else
					memcpy(out, in, width - col);
			}
		}
	}
}

static void vdec_p010_row(__le16 *dst, const u8 *high, const u8 *low,
			  unsigned int count, bool chroma)
{
	unsigned int i;

	/* Expand four high bytes and four packed low pairs into 16-bit lanes. */
	for (i = 0; i < count; i += 4) {
		/* Copy the packed byte at shifts 0, 14, 28 and 42. */
		u64 msb = get_unaligned_le32(high + i);
		u64 lsb = (u64)low[i / 4] * 0x0000040010004001ULL;
		u64 samples;

		msb = (msb | msb << 16) & 0x0000ffff0000ffffULL;
		msb = (msb | msb << 8) & 0x00ff00ff00ff00ffULL;
		samples = msb << 8 | (lsb & 0x0003000300030003ULL) << 6;
		/* VDH stores VU, while P010 uses UV. */
		if (chroma)
			samples = (samples & 0x0000ffff0000ffffULL) << 16 |
				  (samples & 0xffff0000ffff0000ULL) >> 16;
		put_unaligned_le64(samples, dst + i);
	}
}

void vdec_p010_pack_neon(__le16 *dst, const u8 *high, const u8 *low,
			unsigned int count, bool chroma);

static void vdec_detile_p010(__le16 *dst, const u8 *src, const u8 *tail,
			    unsigned int width, unsigned int height,
			    unsigned int stride, unsigned int tile_height)
{
	unsigned int band_row, col;
	unsigned int mask = tile_height / 4 - 1;
	bool chroma = tile_height == 8;
	bool simd = may_use_simd();
	struct user_fpsimd_state neon_state;

	for (band_row = 0; band_row < height; band_row += tile_height) {
		const u8 *tile = src + band_row * stride;
		const u8 *low = tail + band_row * (stride / 4);
		__le16 *band = dst + band_row * width;
		unsigned int rows = min(tile_height, height - band_row);

		if (simd)
			kernel_neon_begin(&neon_state);
		for (col = 0; col < width; col += 64, tile += 64 * tile_height, low += 32) {
			unsigned int swizzle = (col / 64 & mask) * 4;
			unsigned int count = min(64U, width - col);
			unsigned int vector_count = simd ? round_down(count, 16U) : 0;
			unsigned int row;

			for (row = 0; row < rows; row++) {
				const u8 *high = tile + (row ^ swizzle) * 64;
				const u8 *packed = low + (row & ~1U) * (stride / 4) + (row & 1) * 16;
				__le16 *out = band + row * width + col;

				if (vector_count)
					vdec_p010_pack_neon(out, high, packed, vector_count, chroma);
				if (vector_count < count)
					vdec_p010_row(out + vector_count, high + vector_count,
						      packed + vector_count / 4,
						      count - vector_count, chroma);
			}
		}
		if (simd)
			kernel_neon_end(&neon_state);
	}
}

static int vdec_copy_rbsp(struct vdec_ctx *ctx, u8 *dst, const u8 *src, unsigned int size,
			  unsigned int header)
{
	unsigned int i, length = header, zeroes = 0;
	ktime_t start = ktime_get();

	/* Keep the start code and NAL header ahead of the VDH's bit offset. */
	memcpy(dst, src, length);
	for (i = length; i < size; i++) {
		u8 byte = src[i];

		if (zeroes == 2) {
			/* Slice mode accepts one VCL NAL per request. */
			if (byte == 1)
				return -EINVAL;
			if (byte == 3) {
				zeroes = 0;
				continue;
			}
		}
		dst[length++] = byte;
		zeroes = byte ? 0 : min(zeroes + 1, 2U);
	}
	ctx->picture_rbsp_ns += ktime_sub(ktime_get(), start);
	return length;
}

#define REQUEST_PICTURE		BIT(0)
#define REQUEST_SCALING		BIT(1)
#define REQUEST_WEIGHTS		BIT(2)

static int vdec_finish_picture(struct vdec_ctx *ctx, struct vb2_v4l2_buffer *dst)
{
	struct vdec_buffer *out = to_vdec_buffer(&dst->vb2_buf);
	unsigned int width = ctx->output.width, height = ctx->output.height;
	u8 *output = vb2_plane_vaddr(&dst->vb2_buf, 0);
	struct vdec_timing *timing = &ctx->timing;
	ktime_t start;
	u64 clear_ns, hw_ns;
	int ret;

	ctx->codec->end_picture(ctx);
	if (ctx->vdec->dead)
		return -EIO;

	vdec_surface_sync(ctx, &out->surface, false);
	start = ktime_get();
	if (out->native_pin) {
		memset(output, 0, PAGE_ALIGN(ctx->capture.plane_fmt[0].sizeimage));
		memset(out->surface.cpu, 0, out->surface.size);
		dma_wmb();
	} else {
		memset(out->surface.cpu, 0, out->surface.size);
	}
	clear_ns = ktime_sub(ktime_get(), start);
	vdec_surface_sync(ctx, &out->surface, true);
	ret = vdec_submit(ctx, &hw_ns);
	ctx->num_slices = 0;
	if (ret)
		return ret;
	if (out->native_pin)
		goto complete;

	vdec_surface_sync(ctx, &out->surface, false);
	start = ktime_get();
	if (ctx->tail_size) {
		const u8 *tail = out->surface.cpu + ctx->y_size + ctx->vu_size;

		vdec_detile_p010((__le16 *)output, out->surface.cpu, tail,
				width, height, ctx->stride, 16);
		vdec_detile_p010((__le16 *)output + width * height,
				out->surface.cpu + ctx->y_size, tail + ctx->y_size / 4,
				width, height / 2, ctx->stride, 8);
	} else {
		vdec_detile_plane(output, out->surface.cpu, width, height, ctx->stride, 16);
		vdec_detile_plane(output + width * height, out->surface.cpu + ctx->y_size,
				  width, height / 2, ctx->stride, 8);
	}
	timing->detile_ns += ktime_sub(ktime_get(), start);
complete:
	timing->rbsp_ns += ctx->picture_rbsp_ns;
	timing->hw_ns += hw_ns;
	timing->clear_ns += clear_ns;
	timing->sync_ns += ctx->picture_sync_ns;
	timing->frames++;
	ctx->picture_rbsp_ns = 0;
	ctx->picture_sync_ns = 0;
	out->decoded = true;
	out->timestamp = ctx->held_timestamp;
	vb2_set_plane_payload(&dst->vb2_buf, 0, ctx->capture.plane_fmt[0].sizeimage);
	return 0;
}

static void vdec_h264_end_picture(struct vdec_ctx *ctx)
{
	unsigned int mb_width = ctx->output.width / 16;
	__le32 *slice = ctx->workspace.cpu + vdec_slice_offset(ctx->num_slices - 1);

	vdec_slice_end(slice, mb_width * (ctx->output.height / 16) - 1, mb_width);
}

static int vdec_h264_prepare_workspace(struct vdec_ctx *ctx)
{
	int ret;

	ret = vdec_dma_alloc(ctx->vdec, &ctx->workspace,
			     STREAM_OFFSET + ctx->output.plane_fmt[0].sizeimage +
			     VDEC_STREAM_PAD, false);
	if (!ret)
		memcpy(ctx->workspace.cpu + CABAC_TABLE, h264_cabac_init,
		       sizeof(h264_cabac_init));
	return ret;
}

static int vdec_h264_pack_request(struct vdec_ctx *ctx, struct vb2_v4l2_buffer *src,
				 struct vb2_v4l2_buffer *dst, unsigned int controls)
{
	const struct v4l2_ctrl_h264_sps *sps = ctx->sps->p_cur.p_h264_sps;
	const struct v4l2_ctrl_h264_pps *pps = ctx->pps->p_cur.p_h264_pps;
	const struct v4l2_ctrl_h264_slice_params *sp = ctx->slice->p_cur.p_h264_slice_params;
	const struct v4l2_ctrl_h264_decode_params *dec = ctx->decode->p_cur.p_h264_decode_params;
	struct vdec_refs refs = {};
	struct vdec_buffer *out = to_vdec_buffer(&dst->vb2_buf);
	unsigned int width = ctx->output.width, height = ctx->output.height;
	unsigned int mbs = (width / 16) * (height / 16);
	unsigned int offset = src->planes[0].data_offset;
	unsigned int bytes = vb2_get_plane_payload(&src->vb2_buf, 0) - offset;
	unsigned int prefix, slice_offset, bit;
	bool cabac;
	const u8 *input = vb2_plane_vaddr(&src->vb2_buf, 0);
	u8 *output = vb2_plane_vaddr(&dst->vb2_buf, 0);
	__le32 *slice;
	u8 *stream;
	int ret;

	if (ctx->num_slices && (ctx->held != out || ctx->held_timestamp != src->vb2_buf.timestamp))
		ctx->num_slices = 0;
	if (!ctx->num_slices) {
		if (sp->first_mb_in_slice || !(controls & REQUEST_PICTURE) ||
		    ((pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) &&
		     !(controls & REQUEST_SCALING)))
			return -EINVAL;
		ctx->held = out;
		ctx->held_timestamp = src->vb2_buf.timestamp;
		ctx->picture_rbsp_ns = 0;
		ctx->picture_sync_ns = 0;
		ctx->stream_used = 0;
		ctx->picture_sps = *sps;
		ctx->picture_pps = *pps;
		ctx->picture_dec = *dec;
	} else if (sp->first_mb_in_slice <= ctx->last_mb ||
		   ((controls & REQUEST_PICTURE) &&
		    (memcmp(pps, &ctx->picture_pps, sizeof(*pps)) ||
		     memcmp(dec, &ctx->picture_dec, sizeof(*dec))))) {
		return -EINVAL;
	}
	sps = &ctx->picture_sps;
	pps = &ctx->picture_pps;
	dec = &ctx->picture_dec;
	cabac = pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	out->decoded = false;
	if (ctx->vdec->dead)
		return -EIO;
	if (!input || !output || bytes < 5 || !out->surface.cpu ||
	    (sps->pic_width_in_mbs_minus1 + 1) * 16 != width ||
	    (sps->pic_height_in_map_units_minus1 + 1) * 16 != height ||
	    !(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ||
	    sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 ||
	    sps->bit_depth_chroma_minus8 || pps->num_slice_groups_minus1 ||
	    pps->seq_parameter_set_id != sps->seq_parameter_set_id ||
	    (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC) ||
	    sp->first_mb_in_slice >= mbs)
		return -EINVAL;

	input += offset;
	if (!memcmp(input, "\x00\x00\x01", 3))
		prefix = 3;
	else if (!memcmp(input, "\x00\x00\x00\x01", 4))
		prefix = 4;
	else
		return -EINVAL;
	if ((input[prefix] & 0x1f) !=
	    (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC ? 5 : 1))
		return -EINVAL;
	if (sp->header_bit_size < 8 || sp->header_bit_size >= (bytes - prefix) * 8)
		return -EINVAL;
	bit = sp->header_bit_size;
	if (cabac)
		bit = ALIGN(bit, 8);
	bit += prefix * 8;

	if (ctx->num_slices == VDEC_MAX_SLICES ||
	    ctx->stream_used + bytes > ctx->output.plane_fmt[0].sizeimage)
		return -ENOSPC;

	ret = vdec_h264_find_refs(ctx, out, dec, &refs);
	if (ret)
		return ret;
	if (!ctx->workspace.cpu) {
		ret = ctx->codec->prepare_workspace(ctx);
		if (ret)
			return ret;
	}
	if (!ctx->num_slices)
		vdec_picture_message(ctx, out, &refs);

	stream = ctx->workspace.cpu + STREAM_OFFSET + ctx->stream_used;
	/* The VDH entropy decoder consumes RBSP at the slice data offset. */
	ret = vdec_copy_rbsp(ctx, stream, input, bytes, prefix + 1);
	if (ret < 0)
		return ret;
	bytes = ret;
	if (bit >= bytes * 8)
		return -EINVAL;
	memset(stream + bytes, 0, VDEC_STREAM_PAD);

	slice_offset = vdec_slice_offset(ctx->num_slices);
	slice = ctx->workspace.cpu + slice_offset;
	memset(slice, 0, SLICE_MSG_SIZE);
	ret = vdec_slice_message(ctx, slice, &refs, ctx->stream_used * 8 + bit,
				  bytes * 8 - bit, controls & REQUEST_WEIGHTS);
	if (ret)
		return ret;
	if (ctx->num_slices) {
		__le32 *prev = ctx->workspace.cpu + vdec_slice_offset(ctx->num_slices - 1);

		vdec_slice_end(prev, sp->first_mb_in_slice - 1, width / 16);
		prev[0xfc / 4] = vdec_addr(ctx->workspace.iova, slice_offset);
	}
	ctx->num_slices++;
	ctx->stream_used = ALIGN(ctx->stream_used + bytes, 16);
	ctx->last_mb = sp->first_mb_in_slice;
	return 0;
}

static int vdec_hevc_prepare_workspace(struct vdec_ctx *ctx)
{
	int ret;

	ret = vdec_dma_alloc(ctx->vdec, &ctx->workspace,
			     HEVC_STREAM_OFFSET + ctx->output.plane_fmt[0].sizeimage +
			     VDEC_STREAM_PAD, false);
	if (!ret)
		memcpy(ctx->workspace.cpu + CABAC_TABLE, hevc_cabac_init,
		       sizeof(hevc_cabac_init));
	return ret;
}

static int vdec_hevc_tile_boundaries(u16 *start, const u8 *sizes, unsigned int count,
				    unsigned int ctus, bool uniform)
{
	unsigned int i;

	if (count > ctus)
		return -EINVAL;
	start[0] = 0;
	for (i = 1; i < count; i++) {
		start[i] = uniform ? i * ctus / count : start[i - 1] + sizes[i - 1] + 1;
		if (start[i] >= ctus)
			return -EINVAL;
	}
	start[count] = ctus;
	return 0;
}

static int vdec_hevc_tile_layout(struct vdec_ctx *ctx, unsigned int width, unsigned int height)
{
	const struct v4l2_ctrl_hevc_pps *pps = &ctx->hevc_picture_pps;
	struct vdec_hevc_tiles *tiles = &ctx->hevc_tiles;
	bool enabled = pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
	bool uniform = pps->flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING;
	int ret;

	tiles->cols = enabled ? pps->num_tile_columns_minus1 + 1 : 1;
	tiles->rows = enabled ? pps->num_tile_rows_minus1 + 1 : 1;
	if (tiles->cols > HEVC_MAX_TILE_COLUMNS || tiles->rows > HEVC_MAX_TILE_ROWS)
		return -EINVAL;
	ret = vdec_hevc_tile_boundaries(tiles->col, pps->column_width_minus1,
					tiles->cols, width, uniform);
	if (ret)
		return ret;
	return vdec_hevc_tile_boundaries(tiles->row, pps->row_height_minus1,
					 tiles->rows, height, uniform);
}

static void vdec_hevc_tile_axis(u8 *map, __le16 *bounds, const u16 *start,
			       unsigned int count, unsigned int units)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		memset(map + start[i] * units, i, (start[i + 1] - start[i]) * units);
		bounds[2 * i] = cpu_to_le16(start[i]);
		bounds[2 * i + 1] = cpu_to_le16(start[i + 1] - 1);
	}
}

static unsigned int vdec_hevc_to_tile_scan(const struct vdec_hevc_tiles *tiles,
					  unsigned int addr)
{
	unsigned int width = tiles->col[tiles->cols];
	unsigned int x = addr % width, y = addr / width;
	unsigned int col = 0, row = 0, w, h;

	while (x >= tiles->col[col + 1])
		col++;
	while (y >= tiles->row[row + 1])
		row++;
	w = tiles->col[col + 1] - tiles->col[col];
	h = tiles->row[row + 1] - tiles->row[row];
	return width * tiles->row[row] + h * tiles->col[col] +
	       w * (y - tiles->row[row]) + x - tiles->col[col];
}

static unsigned int vdec_hevc_to_raster(const struct vdec_hevc_tiles *tiles,
					unsigned int addr)
{
	unsigned int width = tiles->col[tiles->cols];
	unsigned int col = 0, row = 0, w, h;

	while (addr >= width * tiles->row[row + 1])
		row++;
	addr -= width * tiles->row[row];
	h = tiles->row[row + 1] - tiles->row[row];
	while (addr >= h * tiles->col[col + 1])
		col++;
	addr -= h * tiles->col[col];
	w = tiles->col[col + 1] - tiles->col[col];
	return (tiles->row[row] + addr / w) * width + tiles->col[col] + addr % w;
}

static void vdec_hevc_end_slice(struct vdec_ctx *ctx, __le32 *slice, unsigned int end,
			       bool last_independent)
{
	const struct vdec_hevc_tiles *tiles = &ctx->hevc_tiles;
	unsigned int raster = vdec_hevc_to_raster(tiles, end);
	unsigned int row = raster / tiles->col[tiles->cols];

	slice[43] = cpu_to_le32(raster);
	slice[44] = cpu_to_le32(end);
	slice[62] = cpu_to_le32(row | row << 16 | (last_independent ? BIT(31) : 0));
}

static void vdec_hevc_end_picture(struct vdec_ctx *ctx)
{
	const struct vdec_hevc_tiles *tiles = &ctx->hevc_tiles;
	__le32 *core = ctx->workspace.cpu + MSG_CORE;
	unsigned int width = tiles->col[tiles->cols];
	unsigned int height = tiles->row[tiles->rows];
	__le32 *slice = ctx->workspace.cpu + MSG_SLICE +
			(ctx->num_slices - 1) * SLICE_MSG_SIZE;

	vdec_hevc_end_slice(ctx, slice, width * height - 1, true);
	core[36] = cpu_to_le32(ctx->num_slices);
}

static int vdec_hevc_find_refs(struct vdec_ctx *ctx, struct vdec_buffer *dst,
			      const struct v4l2_ctrl_hevc_decode_params *dec,
			      struct vdec_buffer **refs)
{
	unsigned int i;

	if (dec->num_active_dpb_entries > V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		return -EINVAL;
	for (i = 0; i < dec->num_active_dpb_entries; i++) {
		if (dec->dpb[i].field_pic)
			return -EINVAL;
		refs[i] = vdec_find_buffer(ctx, dst, dec->dpb[i].timestamp);
		if (!refs[i])
			return -ENOENT;
	}
	return 0;
}

static void vdec_hevc_scaling_list(u8 *dst, const struct v4l2_ctrl_hevc_scaling_matrix *matrix)
{
	unsigned int i, x, y;

	/* The VDH reads four coefficients per word, vertically within a tile. */
	for (i = 0; i < 6; i++) {
		for (y = 0; y < 8; y++) {
			for (x = 0; x < 8; x++) {
				unsigned int column = x / 4 * 32 + x % 4 * 8;
				unsigned int c = y * 8 + x;

				dst[i * 64 + column + y % 2 * 4 + y / 2] =
					matrix->scaling_list_8x8[i][c];
				dst[0x180 + i * 64 + column + y / 4 * 4 + y % 4] =
					matrix->scaling_list_16x16[i][c];
				if (i < 2)
					dst[0x300 + i * 64 + column + y / 4 * 4 + y % 4] =
						matrix->scaling_list_32x32[i][c];
			}
		}
		for (y = 0; y < 4; y++)
			for (x = 0; x < 4; x++)
				dst[0x380 + i * 16 + x * 4 + y % 2 * 2 + y / 2] =
					matrix->scaling_list_4x4[i][y * 4 + x];
	}
	memcpy(dst + 0x3e0, matrix->scaling_list_dc_coef_16x16, 6);
	memcpy(dst + 0x3e6, matrix->scaling_list_dc_coef_32x32, 2);
}

static void vdec_hevc_picture_message(struct vdec_ctx *ctx, struct vdec_buffer *dst,
				      struct vdec_buffer **refs, unsigned int log2_ctu,
				      unsigned int ctu_width, unsigned int ctu_height)
{
	const struct v4l2_ctrl_hevc_sps *sps = &ctx->hevc_picture_sps;
	const struct v4l2_ctrl_hevc_pps *pps = &ctx->hevc_picture_pps;
	const struct v4l2_ctrl_hevc_decode_params *dec = &ctx->hevc_picture_dec;
	void *work = ctx->workspace.cpu;
	__le32 *pub = work, *core = work + MSG_CORE, *pic = work + MSG_PICTURE;
	u32 base = ctx->workspace.iova, out = dst->surface.iova;
	unsigned int surface_size = ctx->y_size + ctx->vu_size;
	unsigned int tail_offset = ctx->tail_size ? surface_size : 0;
	unsigned int tail_y_size = ctx->tail_size ? ctx->y_size / 4 : 0;
	unsigned int tail_stride = ctx->tail_size ? ctx->stride / 4 : 0;
	unsigned int min_cb = sps->log2_min_luma_coding_block_size_minus3 + 3;
	unsigned int min_tb = sps->log2_min_luma_transform_block_size_minus2 + 2;
	unsigned int width = sps->pic_width_in_luma_samples, height = sps->pic_height_in_luma_samples;
	unsigned int ctu_count = ctu_width * ctu_height;
	u32 geometry = (ctu_height - 1) << 16 | (ctu_width - 1);
	bool scaling = sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED;
	bool tiled = pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
	const struct vdec_hevc_tiles *tiles = &ctx->hevc_tiles;
	unsigned int units = 1U << (log2_ctu - 4);
	unsigned int i;

	memset(work, 0, MSG_SLICE);
	memset(work + MSG_UP, 0, MSG_UP_SIZE);

	pub[2] = cpu_to_le32(BIT(24) | (scaling ? BIT(30) : 0) | (ctu_count - 1));
	pub[3] = cpu_to_le32(0x3c00d);
	pub[4] = vdec_addr(base, MSG_PICTURE);
	pub[5] = vdec_addr(base, MSG_UP);
	pub[6] = vdec_addr(base, HEVC_STREAM_OFFSET);
	pub[8] = cpu_to_le32(1);
	pub[9] = vdec_addr(out, 0);
	pub[10] = cpu_to_le32(ctx->stride);
	pub[12] = cpu_to_le32(ctx->y_size);
	pub[14] = cpu_to_le32(tail_stride);
	pub[15] = cpu_to_le32(tail_offset);
	pub[16] = cpu_to_le32(tail_y_size);
	pub[20] = vdec_addr(out, surface_size + ctx->tail_size);
	pub[22] = vdec_addr(base, HEVC_EXT_WORK + 0x1000);
	pub[23] = vdec_addr(out, ctx->pmv_offset);
	pub[24] = vdec_addr(base, HEVC_EXT_WORK + 0x7000);
	pub[25] = vdec_addr(base, CABAC_TABLE);
	pub[26] = vdec_addr(base, HEVC_EXT_WORK + 0x4000);
	pub[27] = vdec_addr(base, HEVC_EXT_WORK + 0x23100);
	pub[28] = vdec_addr(base, HEVC_EXT_WORK + 0x14900);
	pub[29] = vdec_addr(base, HEVC_EXT_WORK + 0x1f900);
	pub[30] = vdec_addr(base, HEVC_EXT_WORK + 0x9800);
	pub[31] = cpu_to_le32(geometry);
	pub[32] = cpu_to_le32((tiles->cols - 1) | (tiles->rows - 1) << 16);

	core[28] = vdec_addr(base, MSG_SLICE);
	core[32] = vdec_addr(base, MSG_UP + 0x80);
	core[36] = cpu_to_le32(1);
	for (i = 0; i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {
		struct vdec_buffer *ref = refs[i] ?: refs[0] ?: dst;
		u32 addr = ref->surface.iova;

		if (refs[i]) {
			pic[8 + i] = vdec_addr(addr, 0);
			pic[26 + i] = cpu_to_le32(dec->dpb[i].pic_order_cnt_val);
		}
		core[0x500 / 4 + i] = vdec_addr(addr, 0);
		core[0x540 / 4 + i] = vdec_addr(addr, surface_size);
		core[0x580 / 4 + i] = vdec_addr(addr, tail_offset);
	}
	for (i = 0; i < 3; i++) {
		core[0x5cc / 4 + i] = cpu_to_le32(width | height << 16);
		core[0x5d8 / 4 + i] = cpu_to_le32(ctx->y_size);
		core[0x5f0 / 4 + i] = cpu_to_le32(tail_y_size);
		core[0x5fc / 4 + i] = cpu_to_le32(ctx->stride);
		core[0x608 / 4 + i] = cpu_to_le32(tail_stride);
	}
	core[0x614 / 4] = vdec_addr(out, surface_size);
	core[0x61c / 4] = vdec_addr(out, tail_offset);

	pic[0] = cpu_to_le32((ctu_width - 1) | (ctu_height - 1) << 9 | BIT(19) |
			     (sps->flags & V4L2_HEVC_SPS_FLAG_AMP_ENABLED ? BIT(21) : 0) |
			     (sps->flags & V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET ? BIT(22) : 0) |
			     (scaling ? BIT(24) : 0) |
			     (sps->flags & V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED ? BIT(25) : 0));
	pic[1] = cpu_to_le32(dec->pic_order_cnt_val);
	pic[2] = cpu_to_le32(log2_ctu | min_cb << 3 |
			     2 * (log2_ctu - min_cb) << 6 |
			     (min_tb + sps->log2_diff_max_min_luma_transform_block_size) << 9 |
			     min_tb << 12 | (u32)sps->max_transform_hierarchy_depth_intra << 21 |
			     (u32)sps->max_transform_hierarchy_depth_inter << 24 |
			     (pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED ? BIT(27) : 0) |
			     (pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED ? BIT(28) : 0) |
			     (u32)pps->log2_parallel_merge_level_minus2 << 29);
	pic[3] = cpu_to_le32((8 + sps->bit_depth_luma_minus8) |
			     (8 + sps->bit_depth_chroma_minus8) << 4 |
			     (6 * sps->bit_depth_luma_minus8) << 16 |
			     (6 * sps->bit_depth_chroma_minus8) << 22);
	pic[54] = cpu_to_le32(width | height << 16);
	pic[55] = vdec_addr(base, HEVC_TILE_INFO);
	pic[60] = cpu_to_le32((pps->pps_cr_qp_offset & 31) | (pps->pps_cb_qp_offset & 31) << 5 |
			      (pps->flags & V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED ? BIT(10) : 0) |
			      (pps->flags & V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED ? BIT(11) : 0) |
			      (pps->flags & V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED ? BIT(12) : 0) |
			      (u32)pps->diff_cu_qp_delta_depth << 13 |
			      (log2_ctu - pps->diff_cu_qp_delta_depth) << 16 |
			      (tiled ? BIT(19) : 0) |
			      (pps->flags & V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED ? BIT(21) : 0) |
			      (pps->flags & V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED ? BIT(22) : 0) |
			      (pps->flags & V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED ? BIT(23) : 0) |
			      (pps->flags & V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED ? BIT(24) : 0));
	pic[61] = pub[32];
	pic[64] = cpu_to_le32(0x40000000);
	pic[76] = cpu_to_le32(0x08000000);
	if (scaling)
		vdec_hevc_scaling_list((u8 *)(pic + 128), ctx->hevc_scaling->p_cur.p);
	if (tiled) {
		u8 *info = work + HEVC_TILE_INFO;
		__le16 *bounds = (__le16 *)(info + 2 * HEVC_TILE_MAP_SIZE);

		memset(info, 0, HEVC_TILE_INFO_SIZE);
		vdec_hevc_tile_axis(info, bounds, tiles->col, tiles->cols, units);
		vdec_hevc_tile_axis(info + HEVC_TILE_MAP_SIZE, bounds + 2 * HEVC_MAX_TILE_COLUMNS,
				    tiles->row, tiles->rows, units);
	}
}

static int vdec_hevc_ref_list(__le32 *slice, const struct v4l2_ctrl_hevc_decode_params *dec,
			     unsigned int list, const u8 *indices, unsigned int count)
{
	unsigned int i;
	u32 long_term = 0, slots[2] = {};

	if (count > 15)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		unsigned int index = indices[i];

		if (index >= dec->num_active_dpb_entries)
			return -EINVAL;
		if (dec->dpb[index].flags & V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE)
			long_term |= BIT(i);
		slots[i / 8] |= index << (i % 8 * 4);
	}
	slice[13 + list] = cpu_to_le32(long_term);
	slice[15 + 2 * list] = cpu_to_le32(slots[0]);
	slice[16 + 2 * list] = cpu_to_le32(slots[1]);
	return 0;
}

static void vdec_hevc_weights(__le32 *slice, unsigned int count,
			      unsigned int luma_denom, unsigned int chroma_denom,
			      const s8 *luma_weight, const s8 *luma_offset,
			      const s8 (*chroma_weight)[2], const s8 (*chroma_offset)[2])
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		int y = (1 << luma_denom) + luma_weight[i];
		int cb = (1 << chroma_denom) + chroma_weight[i][0];
		int cr = (1 << chroma_denom) + chroma_weight[i][1];

		slice[64 + i] = cpu_to_le32(luma_denom | (y & 511) << 3 |
					    (u32)(u8)luma_offset[i] << 12);
		slice[128 + i] = cpu_to_le32(chroma_denom | (cb & 511) << 3 |
					     (u32)(u8)chroma_offset[i][0] << 12);
		slice[160 + i] = cpu_to_le32((cr & 511) | (u32)(u8)chroma_offset[i][1] << 9 |
					     chroma_denom << 17);
	}
}

static int vdec_hevc_slice_message(struct vdec_ctx *ctx, __le32 *slice,
				  const struct v4l2_ctrl_hevc_slice_params *sp,
				  struct vdec_buffer **refs, struct vdec_buffer *out,
				  unsigned int tile_start, unsigned int data_bit,
				  unsigned int data_bits)
{
	const struct v4l2_ctrl_hevc_pps *pps = &ctx->hevc_picture_pps;
	const struct v4l2_ctrl_hevc_decode_params *dec = &ctx->hevc_picture_dec;
	bool intra = sp->slice_type == V4L2_HEVC_SLICE_TYPE_I;
	bool bipred = sp->slice_type == V4L2_HEVC_SLICE_TYPE_B;
	bool temporal = sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED;
	bool from_l0 = !bipred || sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0;
	unsigned int l0 = intra ? 0 : sp->num_ref_idx_l0_active_minus1 + 1;
	unsigned int l1 = bipred ? sp->num_ref_idx_l1_active_minus1 + 1 : 0;
	unsigned int first = sp->slice_segment_addr, col_index = 0;
	unsigned int ctu_width = ctx->hevc_tiles.col[ctx->hevc_tiles.cols];
	bool no_future = true;
	struct vdec_buffer *col = out;
	int qp = 26 + pps->init_qp_minus26 + sp->slice_qp_delta;
	int qp_min = -6 * ctx->hevc_picture_sps.bit_depth_luma_minus8;
	unsigned int i;
	int ret;

	if (qp < qp_min || qp > 51 || sp->five_minus_max_num_merge_cand > 4)
		return -EINVAL;
	ret = vdec_hevc_ref_list(slice, dec, 0, sp->ref_idx_l0, l0);
	if (ret)
		return ret;
	ret = vdec_hevc_ref_list(slice, dec, 1, sp->ref_idx_l1, l1);
	if (ret)
		return ret;
	for (i = 0; i < l0; i++)
		no_future &= dec->dpb[sp->ref_idx_l0[i]].pic_order_cnt_val <= dec->pic_order_cnt_val;
	for (i = 0; i < l1; i++)
		no_future &= dec->dpb[sp->ref_idx_l1[i]].pic_order_cnt_val <= dec->pic_order_cnt_val;
	if (temporal && !intra) {
		if (sp->collocated_ref_idx >= (from_l0 ? l0 : l1))
			return -EINVAL;
		col_index = from_l0 ? sp->ref_idx_l0[sp->collocated_ref_idx] :
				      sp->ref_idx_l1[sp->collocated_ref_idx];
		col = refs[col_index];
	}

	slice[0] = cpu_to_le32(data_bit / 128);
	slice[1] = cpu_to_le32(data_bit % 128);
	slice[2] = cpu_to_le32(data_bits);
	slice[6] = cpu_to_le32(first | (qp & 0x7f) << 19 | (no_future ? BIT(27) : 0) |
			       (sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT ? BIT(18) : 0) |
			       (sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT ?
				BIT(26) : 0));
	slice[7] = cpu_to_le32((V4L2_HEVC_SLICE_TYPE_I - sp->slice_type) | l0 << 2 | l1 << 8 |
			       (temporal ? BIT(15) : 0) | (from_l0 ? BIT(27) : 0) |
			       (sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO ? BIT(14) : 0) |
			       (intra ? 0 : (5U - sp->five_minus_max_num_merge_cand) << 24) |
			       (l0 ? (l0 - 1) << 16 : 0) | (l1 ? (l1 - 1) << 20 : 0) |
			       col_index << 28);
	slice[8] = cpu_to_le32(first);
	slice[9] = cpu_to_le32(first % ctu_width | (first / ctu_width) << 16);
	slice[10] = cpu_to_le32(tile_start);
	slice[11] = cpu_to_le32((sp->slice_cb_qp_offset & 31) << 8 |
				(sp->slice_cr_qp_offset & 31));
	slice[12] = cpu_to_le32((sp->slice_beta_offset_div2 & 15) << 8 |
				(sp->slice_tc_offset_div2 & 15) << 16 |
				(sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA ? BIT(3) : 0) |
				(sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA ? BIT(2) : 0) |
				(sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED ?
				 BIT(1) : 0) |
				(sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED ?
				 BIT(0) : 0));
	slice[23] = vdec_addr(col->surface.iova, ctx->pmv_offset);
	slice[24] = cpu_to_le32(0x400a);
	if (!intra &&
	    (pps->flags & (bipred ? V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED :
				  V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED))) {
		const struct v4l2_hevc_pred_weight_table *weights = &sp->pred_weight_table;
		int luma_denom = weights->luma_log2_weight_denom;
		int chroma_denom = luma_denom + weights->delta_chroma_log2_weight_denom;

		if (luma_denom > 7 || chroma_denom < 0 || chroma_denom > 7)
			return -EINVAL;
		vdec_hevc_weights(slice, l0, luma_denom, chroma_denom,
				  weights->delta_luma_weight_l0, weights->luma_offset_l0,
				  weights->delta_chroma_weight_l0, weights->chroma_offset_l0);
		vdec_hevc_weights(slice + 16, l1, luma_denom, chroma_denom,
				  weights->delta_luma_weight_l1, weights->luma_offset_l1,
				  weights->delta_chroma_weight_l1, weights->chroma_offset_l1);
	}
	return 0;
}

static int vdec_hevc_pack_request(struct vdec_ctx *ctx, struct vb2_v4l2_buffer *src,
				 struct vb2_v4l2_buffer *dst, unsigned int controls)
{
	const struct v4l2_ctrl_hevc_sps *sps = ctx->hevc_sps->p_cur.p_hevc_sps;
	const struct v4l2_ctrl_hevc_pps *pps = ctx->hevc_pps->p_cur.p_hevc_pps;
	const struct v4l2_ctrl_hevc_decode_params *dec = ctx->hevc_decode->p_cur.p;
	const struct v4l2_ctrl_hevc_slice_params *sp = ctx->hevc_slice->p_cur.p_hevc_slice_params;
	struct vdec_buffer *out = to_vdec_buffer(&dst->vb2_buf);
	struct vdec_buffer *refs[V4L2_HEVC_DPB_ENTRIES_NUM_MAX] = {};
	unsigned int offset = src->planes[0].data_offset;
	unsigned int bytes = vb2_get_plane_payload(&src->vb2_buf, 0) - offset;
	unsigned int depth = ctx->tail_size ? 2 : 0;
	unsigned int prefix, ctu_width, ctu_height, log2_ctu, bit, slice_offset, tile_start;
	const u8 *input = vb2_plane_vaddr(&src->vb2_buf, 0);
	__le32 *slice;
	u8 *stream;
	int ret;

	if (ctx->num_slices && (ctx->held != out || ctx->held_timestamp != src->vb2_buf.timestamp))
		ctx->num_slices = 0;
	if (!ctx->num_slices) {
		if (sp->slice_segment_addr || !(controls & REQUEST_PICTURE) ||
		    sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT)
			return -EINVAL;
		ctx->held = out;
		ctx->held_timestamp = src->vb2_buf.timestamp;
		ctx->picture_rbsp_ns = 0;
		ctx->picture_sync_ns = 0;
		ctx->stream_used = 0;
		ctx->hevc_picture_sps = *sps;
		ctx->hevc_picture_pps = *pps;
		ctx->hevc_picture_dec = *dec;
	} else if ((controls & REQUEST_PICTURE) &&
		    (memcmp(sps, &ctx->hevc_picture_sps, sizeof(*sps)) ||
		     memcmp(pps, &ctx->hevc_picture_pps, sizeof(*pps)) ||
		     memcmp(dec, &ctx->hevc_picture_dec, sizeof(*dec)))) {
		return -EINVAL;
	}
	sps = &ctx->hevc_picture_sps;
	pps = &ctx->hevc_picture_pps;
	dec = &ctx->hevc_picture_dec;
	out->decoded = false;
	if (ctx->vdec->dead)
		return -EIO;
	if (!input || !vb2_plane_vaddr(&dst->vb2_buf, 0) || !out->surface.cpu || bytes < 6 ||
	    ctx->hevc_slice->elems != 1 ||
	    sps->pic_width_in_luma_samples != ctx->output.width ||
	    sps->pic_height_in_luma_samples != ctx->output.height ||
	    sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 != depth ||
	    sps->bit_depth_chroma_minus8 != depth ||
	    (sps->flags & (V4L2_HEVC_SPS_FLAG_PCM_ENABLED | V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE)) ||
	    (sp->num_entry_point_offsets &&
	     !(pps->flags & (V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED |
			    V4L2_HEVC_PPS_FLAG_TILES_ENABLED))) ||
	    sp->slice_type > V4L2_HEVC_SLICE_TYPE_I ||
	    sp->colour_plane_id || sp->pic_struct || sp->slice_pic_order_cnt != dec->pic_order_cnt_val ||
	    sp->nal_unit_type > 31 || !sp->nuh_temporal_id_plus1 ||
	    sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_USE_INTEGER_MV)
		return -EINVAL;
	if (!ctx->num_slices && sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED &&
	    !(controls & REQUEST_SCALING))
		return -ENOENT;
	if (sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT &&
	    !(pps->flags & V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED))
		return -EINVAL;

	log2_ctu = sps->log2_min_luma_coding_block_size_minus3 + 3 +
		   sps->log2_diff_max_min_luma_coding_block_size;
	if (log2_ctu < 4 || log2_ctu > 6 ||
	    sps->log2_min_luma_transform_block_size_minus2 + 2 >=
	    sps->log2_min_luma_coding_block_size_minus3 + 3 ||
	    sps->log2_min_luma_transform_block_size_minus2 + 2 +
	    sps->log2_diff_max_min_luma_transform_block_size > min(log2_ctu, 5U) ||
	    sps->max_transform_hierarchy_depth_inter > 4 ||
	    sps->max_transform_hierarchy_depth_intra > 4 ||
	    pps->diff_cu_qp_delta_depth > log2_ctu - 3 ||
	    pps->log2_parallel_merge_level_minus2 > log2_ctu - 2)
		return -EINVAL;
	ctu_width = DIV_ROUND_UP(ctx->output.width, 1U << log2_ctu);
	ctu_height = DIV_ROUND_UP(ctx->output.height, 1U << log2_ctu);
	/* The PXP does not complete a picture with only one CTU row. */
	if (ctu_height < 2)
		return -EOPNOTSUPP;
	if (sp->slice_segment_addr >= ctu_width * ctu_height ||
	    sp->num_entry_point_offsets >= ctu_width * ctu_height)
		return -EINVAL;
	if (!ctx->num_slices) {
		ret = vdec_hevc_tile_layout(ctx, ctu_width, ctu_height);
		if (ret)
			return ret;
	}
	tile_start = vdec_hevc_to_tile_scan(&ctx->hevc_tiles, sp->slice_segment_addr);
	if (ctx->num_slices && tile_start <= ctx->last_mb)
		return -EINVAL;

	input += offset;
	if (!memcmp(input, "\x00\x00\x01", 3))
		prefix = 3;
	else if (!memcmp(input, "\x00\x00\x00\x01", 4))
		prefix = 4;
	else
		return -EINVAL;
	if (input[prefix] != sp->nal_unit_type << 1 || input[prefix + 1] != sp->nuh_temporal_id_plus1 ||
	    sp->data_byte_offset < prefix + 2 || sp->data_byte_offset >= bytes)
		return -EINVAL;
	/* Clients report either the whole NAL span or the slice payload span. */
	if (sp->bit_size != bytes * 8 &&
	    sp->bit_size != (bytes - sp->data_byte_offset) * 8)
		return -EINVAL;
	/* Annex B data_byte_offset includes the prefix and the RBSP header. */
	bit = sp->data_byte_offset * 8;
	if (ctx->num_slices == HEVC_MAX_SLICES ||
	    ctx->stream_used + bytes > ctx->output.plane_fmt[0].sizeimage)
		return -ENOSPC;
	ret = vdec_hevc_find_refs(ctx, out, dec, refs);
	if (ret)
		return ret;
	if (!ctx->workspace.cpu) {
		ret = ctx->codec->prepare_workspace(ctx);
		if (ret)
			return ret;
	}
	if (!ctx->num_slices)
		vdec_hevc_picture_message(ctx, out, refs, log2_ctu, ctu_width, ctu_height);

	stream = ctx->workspace.cpu + HEVC_STREAM_OFFSET + ctx->stream_used;
	ret = vdec_copy_rbsp(ctx, stream, input, bytes, prefix + 2);
	if (ret < 0)
		return ret;
	bytes = ret;
	if (bit >= bytes * 8)
		return -EINVAL;
	memset(stream + bytes, 0, VDEC_STREAM_PAD);

	slice_offset = MSG_SLICE + ctx->num_slices * SLICE_MSG_SIZE;
	slice = ctx->workspace.cpu + slice_offset;
	memset(slice, 0, SLICE_MSG_SIZE);
	ret = vdec_hevc_slice_message(ctx, slice, sp, refs, out, tile_start,
				      ctx->stream_used * 8 + bit, bytes * 8 - bit);
	if (ret)
		return ret;
	if (ctx->num_slices) {
		__le32 *prev = slice - SLICE_MSG_SIZE / sizeof(*slice);

		vdec_hevc_end_slice(ctx, prev, tile_start - 1,
				    !(sp->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT));
		prev[63] = vdec_addr(ctx->workspace.iova, slice_offset);
	}
	ctx->num_slices++;
	ctx->stream_used = ALIGN(ctx->stream_used + bytes, 16);
	ctx->last_mb = tile_start;
	return 0;
}

static unsigned int vdec_hevc_request_controls(struct v4l2_ctrl_handler *hdl)
{
	unsigned int controls = 0;

	if (v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_HEVC_PPS) &&
	    v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS))
		controls |= REQUEST_PICTURE;
	if (v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_HEVC_SCALING_MATRIX))
		controls |= REQUEST_SCALING;
	return controls;
}

static unsigned int vdec_h264_request_controls(struct v4l2_ctrl_handler *hdl)
{
	unsigned int controls = 0;

	if (v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_H264_PPS) &&
	    v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_H264_DECODE_PARAMS))
		controls |= REQUEST_PICTURE;
	if (v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_H264_SCALING_MATRIX))
		controls |= REQUEST_SCALING;
	if (v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_H264_PRED_WEIGHTS))
		controls |= REQUEST_WEIGHTS;
	return controls;
}

static unsigned int vdec_request_controls(struct vdec_ctx *ctx, struct media_request *req)
{
	struct v4l2_ctrl_handler *hdl = v4l2_ctrl_request_hdl_find(req, &ctx->controls);
	unsigned int controls;

	if (!hdl)
		return 0;
	controls = ctx->codec->request_controls(hdl);
	v4l2_ctrl_request_hdl_put(hdl);
	return controls;
}

static void vdec_worker(struct work_struct *work)
{
	struct vdec_dev *vdec = container_of(work, struct vdec_dev, work);
	struct vdec_ctx *ctx = v4l2_m2m_get_curr_priv(vdec->m2m);
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	struct media_request *req = src->vb2_buf.req_obj.req;
	unsigned int controls = vdec_request_controls(ctx, req);
	int ret;

	src->sequence = ctx->sequence_out++;
	dst->sequence = ctx->sequence_cap;
	v4l2_m2m_buf_copy_metadata(src, dst);
	dst->field = V4L2_FIELD_NONE;
	vb2_set_plane_payload(&dst->vb2_buf, 0, 0);

	mutex_lock(&vdec->hw_lock);
	ret = v4l2_ctrl_request_setup(req, &ctx->controls);
	if (!ret) {
		mutex_lock(ctx->controls.lock);
		ret = ctx->codec->pack_request(ctx, src, dst, controls);
		if (!ret && !(src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF))
			ret = vdec_finish_picture(ctx, dst);
		mutex_unlock(ctx->controls.lock);
	}
	if (ret) {
		ctx->num_slices = 0;
		ctx->picture_rbsp_ns = 0;
		ctx->picture_sync_ns = 0;
		src->flags &= ~V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
		dev_dbg(vdec->dev, "request decode returned %d\n", ret);
	}
	if (!(src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF))
		ctx->sequence_cap++;

	v4l2_ctrl_request_complete(req, &ctx->controls);
	v4l2_m2m_buf_done_and_job_finish(vdec->m2m, ctx->fh.m2m_ctx,
					 ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	mutex_unlock(&vdec->hw_lock);
}

static void vdec_device_run(void *priv)
{
	struct vdec_ctx *ctx = priv;

	schedule_work(&ctx->vdec->work);
}

static const struct v4l2_m2m_ops vdec_m2m_ops = {
	.device_run = vdec_device_run,
};

static int vdec_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
			    unsigned int *num_planes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vq);
	const struct v4l2_pix_format_mplane *pix =
		V4L2_TYPE_IS_OUTPUT(vq->type) ? &ctx->output : &ctx->capture;
	unsigned int size = pix->plane_fmt[0].sizeimage;

	if (!V4L2_TYPE_IS_OUTPUT(vq->type) && vdec_is_tiled(pix->pixelformat) &&
	    *num_planes && PAGE_ALIGN(sizes[0]) != PAGE_ALIGN(size))
		return -EINVAL;
	if (*num_planes)
		return *num_planes == 1 && sizes[0] >= size ? 0 : -EINVAL;
	*num_planes = 1;
	sizes[0] = size;
	return 0;
}

/* Map the exportable pixels and private guard/PMV into adjacent VDH IOVAs. */
static int vdec_native_init(struct vdec_ctx *ctx, struct vb2_buffer *vb)
{
	struct vdec_buffer *buf = to_vdec_buffer(vb);
	struct vdec_dev *vdec = ctx->vdec;
	struct vdec_dma *aux = &buf->surface;
	size_t pixels = ALIGN(ctx->capture.plane_fmt[0].sizeimage, VDEC_PAGE);
	size_t total = ALIGN(ctx->pmv_offset + ctx->pmv_size, VDEC_PAGE);
	dma_addr_t dma = vb2_dma_contig_plane_dma_addr(vb, 0);
	int ret;

	if (vdec->dead)
		return -EIO;
	if (!IS_ALIGNED(dma, VDEC_PAGE) || !vb2_plane_vaddr(vb, 0))
		return -EFAULT;
	/* Keep the pages pinned even if the walker cannot be stopped on failure. */
	buf->native_pin = vb2_dma_contig_memops.get_dmabuf(vb, vb->planes[0].mem_priv, O_RDWR);
	if (IS_ERR_OR_NULL(buf->native_pin)) {
		ret = buf->native_pin ? PTR_ERR(buf->native_pin) : -ENOMEM;
		buf->native_pin = NULL;
		return ret;
	}
	aux->iova = gen_pool_alloc(vdec->iova_pool, total);
	if (!aux->iova) {
		ret = -ENOMEM;
		goto unpin;
	}
	aux->size = total - pixels;
	aux->cpu = dma_alloc_coherent(vdec->dev, aux->size, &aux->dma, GFP_KERNEL);
	if (!aux->cpu) {
		ret = -ENOMEM;
		goto free_iova;
	}
	vdec_map_pages(vdec, aux->iova, dma, pixels);
	vdec_map_pages(vdec, aux->iova + pixels, aux->dma, aux->size);
	dma_wmb();
	return 0;

free_iova:
	gen_pool_free(vdec->iova_pool, aux->iova, total);
unpin:
	dma_buf_put(buf->native_pin);
	buf->native_pin = NULL;
	return ret;
}

static int vdec_buf_init(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vdec_buffer *buf = to_vdec_buffer(vb);
	int ret;

	if (V4L2_TYPE_IS_OUTPUT(vb->type))
		return 0;

	/* A surface and its motion vectors live as long as the CAPTURE index. */
	mutex_lock(&ctx->vdec->hw_lock);
	if (vdec_is_tiled(ctx->capture.pixelformat))
		ret = vdec_native_init(ctx, vb);
	else
		ret = vdec_dma_alloc(ctx->vdec, &buf->surface, ctx->pmv_offset + ctx->pmv_size, true);
	mutex_unlock(&ctx->vdec->hw_lock);
	return ret;
}

static void vdec_buf_cleanup(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vdec_buffer *buf = to_vdec_buffer(vb);

	mutex_lock(&ctx->vdec->hw_lock);
	if (buf->native_pin) {
		size_t total = ALIGN(ctx->pmv_offset + ctx->pmv_size, VDEC_PAGE);

		if (vdec_unmap(ctx->vdec, buf->surface.iova, total)) {
			vdec_dma_release(ctx->vdec, &buf->surface);
			dma_buf_put(buf->native_pin);
		}
	} else {
		vdec_dma_free(ctx->vdec, &buf->surface);
	}
	mutex_unlock(&ctx->vdec->hw_lock);
}

static int vdec_buf_prepare(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	if (V4L2_TYPE_IS_OUTPUT(vb->type)) {
		unsigned int offset = to_vb2_v4l2_buffer(vb)->planes[0].data_offset;
		unsigned int used = vb2_get_plane_payload(vb, 0);

		if (used <= offset || used - offset > ctx->output.plane_fmt[0].sizeimage)
			return -EINVAL;
	} else if (vb2_plane_size(vb, 0) < ctx->capture.plane_fmt[0].sizeimage) {
		return -EINVAL;
	}
	return 0;
}

static int vdec_buf_out_validate(struct vb2_buffer *vb)
{
	to_vb2_v4l2_buffer(vb)->field = V4L2_FIELD_NONE;
	return 0;
}

static void vdec_buf_queue(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static void vdec_buf_request_complete(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->controls);
}

static int vdec_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vq);

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		ctx->sequence_out = 0;
	else
		ctx->sequence_cap = 0;
	return 0;
}

static void vdec_stop_streaming(struct vb2_queue *vq)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vq);
	struct vdec_timing *timing = &ctx->timing;
	struct vb2_queue *cap = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	bool output = V4L2_TYPE_IS_OUTPUT(vq->type);
	struct vb2_v4l2_buffer *buf;
	unsigned int i;

	while ((buf = output ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			       v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx))) {
		if (output)
			vdec_buf_request_complete(&buf->vb2_buf);
		v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
	}

	mutex_lock(&ctx->vdec->hw_lock);
	for (i = 0; i < cap->max_num_buffers; i++) {
		struct vb2_buffer *vb = vb2_get_buffer(cap, i);

		if (vb)
			to_vdec_buffer(vb)->decoded = false;
	}
	ctx->num_slices = 0;
	ctx->picture_rbsp_ns = 0;
	ctx->picture_sync_ns = 0;
	if (timing->frames) {
		dev_info(ctx->vdec->dev,
			 "%p4cc -> %p4cc %ux%u timing frames=%u avg_ns/frame rbsp=%llu hw=%llu detile=%llu clear=%llu sync=%llu\n",
			 &ctx->output.pixelformat, &ctx->capture.pixelformat,
			 ctx->output.width, ctx->output.height, timing->frames,
			 div_u64(timing->rbsp_ns, timing->frames),
			 div_u64(timing->hw_ns, timing->frames),
			 div_u64(timing->detile_ns, timing->frames),
			 div_u64(timing->clear_ns, timing->frames),
			 div_u64(timing->sync_ns, timing->frames));
		*timing = (struct vdec_timing) {};
	}
	if (output)
		vdec_dma_free(ctx->vdec, &ctx->workspace);
	mutex_unlock(&ctx->vdec->hw_lock);
}

static const struct vb2_ops vdec_queue_ops = {
	.queue_setup = vdec_queue_setup,
	.buf_init = vdec_buf_init,
	.buf_cleanup = vdec_buf_cleanup,
	.buf_prepare = vdec_buf_prepare,
	.buf_out_validate = vdec_buf_out_validate,
	.buf_queue = vdec_buf_queue,
	.buf_request_complete = vdec_buf_request_complete,
	.start_streaming = vdec_start_streaming,
	.stop_streaming = vdec_stop_streaming,
};

static int vdec_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct vdec_ctx *ctx = priv;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src->io_modes = VB2_MMAP;
	src->drv_priv = ctx;
	src->ops = &vdec_queue_ops;
	/* The queues are copied by the CPU. Only private surfaces need DMA. */
	src->mem_ops = &vb2_vmalloc_memops;
	src->buf_struct_size = sizeof(struct vdec_buffer);
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &ctx->vdec->queue_lock;
	src->dev = ctx->vdec->dev;
	src->subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF;
	src->supports_requests = true;
	src->requires_requests = true;
	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst->io_modes = VB2_MMAP;
	dst->drv_priv = ctx;
	dst->ops = &vdec_queue_ops;
	dst->mem_ops = &vb2_vmalloc_memops;
	dst->buf_struct_size = sizeof(struct vdec_buffer);
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &ctx->vdec->queue_lock;
	dst->dev = ctx->vdec->dev;
	return vb2_queue_init(dst);
}

static int vdec_set_hevc_sps(struct v4l2_ctrl *ctrl)
{
	struct vdec_ctx *ctx = container_of(ctrl->handler, struct vdec_ctx, controls);
	const struct v4l2_ctrl_hevc_sps *sps = ctrl->p_new.p_hevc_sps;
	struct vb2_queue *cap = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);

	if (ctx->output.pixelformat == V4L2_PIX_FMT_HEVC_SLICE && !vb2_is_busy(cap) &&
	    !vdec_is_tiled(ctx->capture.pixelformat))
		vdec_set_capture(ctx, sps->bit_depth_luma_minus8 == 2 &&
				 sps->bit_depth_chroma_minus8 == 2 ?
				 V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV21);
	return 0;
}

static const struct v4l2_ctrl_ops vdec_hevc_sps_ops = {
	.s_ctrl = vdec_set_hevc_sps,
};

static const struct v4l2_ctrl_config vdec_ctrls[] = {
	{ .id = V4L2_CID_STATELESS_H264_SPS },
	{ .id = V4L2_CID_STATELESS_H264_PPS },
	{ .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX },
	{ .id = V4L2_CID_STATELESS_H264_SLICE_PARAMS },
	{ .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS },
	{ .id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS },
	{
		.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		.min = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
		.max = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
		.def = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
	}, {
		.id = V4L2_CID_STATELESS_H264_START_CODE,
		.min = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.max = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.def = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
	},
	{
		.id = V4L2_CID_STATELESS_HEVC_SPS,
		.ops = &vdec_hevc_sps_ops,
		.flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	},
	{ .id = V4L2_CID_STATELESS_HEVC_PPS },
	{ .id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX },
	{
		.id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS,
		.flags = V4L2_CTRL_FLAG_DYNAMIC_ARRAY,
		.dims = { 1 },
	},
	{ .id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS },
	{
		.id = V4L2_CID_STATELESS_HEVC_DECODE_MODE,
		.min = V4L2_STATELESS_HEVC_DECODE_MODE_SLICE_BASED,
		.max = V4L2_STATELESS_HEVC_DECODE_MODE_SLICE_BASED,
		.def = V4L2_STATELESS_HEVC_DECODE_MODE_SLICE_BASED,
	}, {
		.id = V4L2_CID_STATELESS_HEVC_START_CODE,
		.min = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
		.max = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
		.def = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
	},
};

static int vdec_init_ctrls(struct vdec_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->controls;
	unsigned int i;
	int ret;

	ret = v4l2_ctrl_handler_init(hdl, ARRAY_SIZE(vdec_ctrls));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(vdec_ctrls); i++)
		v4l2_ctrl_new_custom(hdl, &vdec_ctrls[i], NULL);
	if (hdl->error)
		return hdl->error;

	ctx->sps = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_H264_SPS);
	ctx->pps = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_H264_PPS);
	ctx->scaling = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_H264_SCALING_MATRIX);
	ctx->slice = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_H264_SLICE_PARAMS);
	ctx->decode = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	ctx->weights = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_H264_PRED_WEIGHTS);
	ctx->hevc_sps = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_HEVC_SPS);
	ctx->hevc_pps = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_HEVC_PPS);
	ctx->hevc_scaling = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_HEVC_SCALING_MATRIX);
	ctx->hevc_slice = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_HEVC_SLICE_PARAMS);
	ctx->hevc_decode = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS);
	ctx->fh.ctrl_handler = hdl;
	return 0;
}

static int vdec_request_validate(struct media_request *req)
{
	struct media_request_object *obj;
	struct vdec_ctx *ctx = NULL;
	struct v4l2_ctrl_handler *hdl;
	int ret;

	if (vb2_request_buffer_cnt(req) != 1)
		return -EINVAL;

	list_for_each_entry(obj, &req->objects, list)
		if (vb2_request_object_is_buffer(obj)) {
			struct vb2_buffer *vb = container_of(obj, struct vb2_buffer, req_obj);

			ctx = vb2_get_drv_priv(vb->vb2_queue);
			break;
		}

	if (!ctx)
		return -EINVAL;
	hdl = v4l2_ctrl_request_hdl_find(req, &ctx->controls);
	if (!hdl)
		return -ENOENT;

	ret = 0;
	if (!v4l2_ctrl_request_hdl_ctrl_find(hdl, ctx->codec->slice_control))
		ret = -ENOENT;

	v4l2_ctrl_request_hdl_put(hdl);
	return ret ?: vb2_request_validate(req);
}

static const struct media_device_ops vdec_media_ops = {
	.req_validate = vdec_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static int vdec_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct vdec_dev *vdec = video_drvdata(file);

	strscpy(cap->driver, "hi3798-vdec", sizeof(cap->driver));
	strscpy(cap->card, VDEC_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", dev_name(vdec->dev));
	return 0;
}

static void vdec_fill_output(struct v4l2_pix_format_mplane *pix)
{
	unsigned int size = pix->plane_fmt[0].sizeimage;
	unsigned int step = pix->pixelformat == V4L2_PIX_FMT_HEVC_SLICE ? 8 : 16;

	pix->width = ALIGN(clamp(pix->width, VDEC_MIN_SIZE, VDEC_MAX_WIDTH), step);
	pix->height = ALIGN(clamp(pix->height, VDEC_MIN_SIZE, VDEC_MAX_HEIGHT), step);
	if (pix->pixelformat != V4L2_PIX_FMT_HEVC_SLICE)
		pix->pixelformat = V4L2_PIX_FMT_H264_SLICE;
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = 1;
	pix->flags = 0;
	memset(pix->reserved, 0, sizeof(pix->reserved));
	memset(pix->plane_fmt, 0, sizeof(pix->plane_fmt));
	pix->plane_fmt[0].sizeimage = size ? clamp(size, 4096U, VDEC_MAX_STREAM) :
					    max_t(unsigned int, SZ_64K, pix->width * pix->height);
	if (!pix->colorspace)
		pix->colorspace = V4L2_COLORSPACE_REC709;
}

static void vdec_fill_capture(const struct v4l2_pix_format_mplane *out,
			      struct v4l2_pix_format_mplane *cap, u32 fourcc)
{
	unsigned int bytes = fourcc == V4L2_PIX_FMT_P010 ? 2 : 1;

	*cap = *out;
	cap->pixelformat = fourcc;
	cap->plane_fmt[0].bytesperline = cap->width * bytes;
	cap->plane_fmt[0].sizeimage = cap->plane_fmt[0].bytesperline * cap->height * 3 / 2;
	if (vdec_is_tiled(fourcc)) {
		unsigned int stride = ALIGN(cap->width, 256);
		unsigned int y = stride * ALIGN(cap->height, 64);

		cap->plane_fmt[0].bytesperline = stride;
		cap->plane_fmt[0].sizeimage = y * (fourcc == MV320_TILED_10 ? 15 : 12) / 8;
	}
}

static void vdec_h264_surface_layout(struct vdec_ctx *ctx)
{
	ctx->pmv_offset = ALIGN(ctx->y_size + ctx->vu_size + 1024, VDEC_PAGE);
	ctx->pmv_size = ALIGN(ctx->output.width, 64) * ALIGN(ctx->output.height, 32) / 4 + 256;
}

static const struct vdec_codec_ops vdec_h264_ops = {
	.pixelformat = V4L2_PIX_FMT_H264_SLICE,
	.slice_control = V4L2_CID_STATELESS_H264_SLICE_PARAMS,
	.surface_layout = vdec_h264_surface_layout,
	.prepare_workspace = vdec_h264_prepare_workspace,
	.pack_request = vdec_h264_pack_request,
	.end_picture = vdec_h264_end_picture,
	.request_controls = vdec_h264_request_controls,
};

static void vdec_hevc_surface_layout(struct vdec_ctx *ctx)
{
	unsigned int guard = ctx->tail_size ? 1280 : 1024;

	ctx->pmv_offset = ALIGN(ctx->y_size + ctx->vu_size + ctx->tail_size + guard, VDEC_PAGE);
	ctx->pmv_size = ALIGN(ctx->output.width, 64) * ALIGN(ctx->output.height, 64) / 4 + 256;
}

static const struct vdec_codec_ops vdec_hevc_ops = {
	.pixelformat = V4L2_PIX_FMT_HEVC_SLICE,
	.slice_control = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS,
	.surface_layout = vdec_hevc_surface_layout,
	.prepare_workspace = vdec_hevc_prepare_workspace,
	.pack_request = vdec_hevc_pack_request,
	.end_picture = vdec_hevc_end_picture,
	.request_controls = vdec_hevc_request_controls,
};

static const struct vdec_codec_ops * const vdec_codecs[] = {
	&vdec_h264_ops, &vdec_hevc_ops,
};

static void vdec_set_capture(struct vdec_ctx *ctx, u32 fourcc)
{
	vdec_fill_capture(&ctx->output, &ctx->capture, fourcc);
	ctx->stride = ALIGN(ctx->output.width, 256);
	ctx->y_size = ctx->stride * ALIGN(ctx->output.height, 64);
	ctx->vu_size = ctx->y_size / 2;
	ctx->tail_size = fourcc == V4L2_PIX_FMT_P010 || fourcc == MV320_TILED_10 ?
			 (ctx->y_size + ctx->vu_size) / 4 : 0;
	ctx->codec->surface_layout(ctx);
	if (ctx->fh.m2m_ctx) {
		struct vb2_queue *cap = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);

		cap->mem_ops = vdec_is_tiled(fourcc) ? &vb2_dma_contig_memops : &vb2_vmalloc_memops;
		cap->dma_dir = vdec_is_tiled(fourcc) ? DMA_BIDIRECTIONAL : DMA_FROM_DEVICE;
	}
}

static void vdec_set_output(struct vdec_ctx *ctx, struct v4l2_pix_format_mplane *pix)
{
	ctx->codec = pix->pixelformat == V4L2_PIX_FMT_HEVC_SLICE ? &vdec_hevc_ops : &vdec_h264_ops;
	ctx->output = *pix;
	vdec_set_capture(ctx, V4L2_PIX_FMT_NV21);
}

static int vdec_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	struct vdec_ctx *ctx = file_to_ctx(file);

	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		if (f->index >= ARRAY_SIZE(vdec_codecs))
			return -EINVAL;
		f->pixelformat = vdec_codecs[f->index]->pixelformat;
	} else if (!f->index) {
		f->pixelformat = V4L2_PIX_FMT_NV21;
	} else if (f->index == 1 && ctx->codec == &vdec_hevc_ops) {
		f->pixelformat = V4L2_PIX_FMT_P010;
	} else if (f->index == (ctx->codec == &vdec_hevc_ops ? 2 : 1)) {
		f->pixelformat = MV320_TILED_8;
		strscpy(f->description, "MV320 tiled VU 8-bit", sizeof(f->description));
	} else if (f->index == 3 && ctx->codec == &vdec_hevc_ops) {
		f->pixelformat = MV320_TILED_10;
		strscpy(f->description, "MV320 tiled VU 8+2-bit", sizeof(f->description));
	} else {
		return -EINVAL;
	}
	return 0;
}

static int vdec_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *f)
{
	struct vdec_ctx *ctx = file_to_ctx(file);
	u32 format = f->pixel_format;
	unsigned int step;

	if (f->index || (f->pixel_format != V4L2_PIX_FMT_H264_SLICE &&
			 f->pixel_format != V4L2_PIX_FMT_HEVC_SLICE &&
			 f->pixel_format != V4L2_PIX_FMT_NV21 &&
			 f->pixel_format != V4L2_PIX_FMT_P010 && !vdec_is_tiled(format)))
		return -EINVAL;
	if (format == V4L2_PIX_FMT_NV21 || vdec_is_tiled(format))
		format = ctx->codec->pixelformat;
	step = format == V4L2_PIX_FMT_H264_SLICE ? 16 : 8;
	f->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	f->stepwise.min_width = VDEC_MIN_SIZE;
	f->stepwise.max_width = VDEC_MAX_WIDTH;
	f->stepwise.step_width = step;
	f->stepwise.min_height = VDEC_MIN_SIZE;
	f->stepwise.max_height = VDEC_MAX_HEIGHT;
	f->stepwise.step_height = step;
	return 0;
}

static int vdec_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vdec_ctx *ctx = file_to_ctx(file);

	f->fmt.pix_mp = V4L2_TYPE_IS_OUTPUT(f->type) ? ctx->output : ctx->capture;
	return 0;
}

static int vdec_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vdec_ctx *ctx = file_to_ctx(file);
	u32 fourcc = f->fmt.pix_mp.pixelformat;

	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		vdec_fill_output(&f->fmt.pix_mp);
		return 0;
	}
	if (fourcc != V4L2_PIX_FMT_NV21 && fourcc != MV320_TILED_8 &&
	    ((fourcc != V4L2_PIX_FMT_P010 && fourcc != MV320_TILED_10) ||
	     ctx->codec != &vdec_hevc_ops))
		fourcc = ctx->capture.pixelformat;
	vdec_fill_capture(&ctx->output, &f->fmt.pix_mp, fourcc);
	return 0;
}

static int vdec_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vdec_ctx *ctx = file_to_ctx(file);
	struct vb2_queue *cap = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	struct vb2_queue *out = v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
	unsigned int i;

	if (vb2_is_busy(cap))
		return -EBUSY;
	vdec_try_fmt(file, priv, f);
	if (!V4L2_TYPE_IS_OUTPUT(f->type)) {
		vdec_set_capture(ctx, f->fmt.pix_mp.pixelformat);
		return 0;
	}
	if (vb2_is_streaming(out) || out->uses_requests || out->queued_count)
		return -EBUSY;
	for (i = 0; i < out->max_num_buffers; i++) {
		struct vb2_buffer *vb = vb2_get_buffer(out, i);

		if (vb && vb2_plane_size(vb, 0) < f->fmt.pix_mp.plane_fmt[0].sizeimage)
			return -EBUSY;
	}

	vdec_set_output(ctx, &f->fmt.pix_mp);
	return 0;
}

static int vdec_flush(struct file *file, void *priv, struct v4l2_decoder_cmd *cmd)
{
	struct vdec_ctx *ctx = file_to_ctx(file);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *dst;
	int ret;

	ret = v4l2_m2m_ioctl_stateless_try_decoder_cmd(file, priv, cmd);
	if (ret)
		return ret;

	mutex_lock(&ctx->vdec->hw_lock);
	dst = v4l2_m2m_next_dst_buf(m2m);
	if (ctx->num_slices && dst && dst->is_held && !v4l2_m2m_next_src_buf(m2m)) {
		ret = vdec_finish_picture(ctx, dst);
		dst->is_held = false;
		dst->sequence = ctx->sequence_cap++;
		v4l2_m2m_dst_buf_remove(m2m);
		v4l2_m2m_buf_done(dst, ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	} else {
		ret = v4l2_m2m_ioctl_stateless_decoder_cmd(file, priv, cmd);
	}
	mutex_unlock(&ctx->vdec->hw_lock);
	return ret;
}

static const struct v4l2_ioctl_ops vdec_ioctl_ops = {
	.vidioc_querycap = vdec_querycap,
	.vidioc_enum_fmt_vid_cap = vdec_enum_fmt,
	.vidioc_enum_fmt_vid_out = vdec_enum_fmt,
	.vidioc_enum_framesizes = vdec_enum_framesizes,
	.vidioc_g_fmt_vid_cap_mplane = vdec_g_fmt,
	.vidioc_g_fmt_vid_out_mplane = vdec_g_fmt,
	.vidioc_try_fmt_vid_cap_mplane = vdec_try_fmt,
	.vidioc_try_fmt_vid_out_mplane = vdec_try_fmt,
	.vidioc_s_fmt_vid_cap_mplane = vdec_s_fmt,
	.vidioc_s_fmt_vid_out_mplane = vdec_s_fmt,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_stateless_try_decoder_cmd,
	.vidioc_decoder_cmd = vdec_flush,
};

static int vdec_open(struct file *file)
{
	struct vdec_dev *vdec = video_drvdata(file);
	struct v4l2_pix_format_mplane pix = {
		.width = VDEC_MIN_SIZE,
		.height = VDEC_MIN_SIZE,
	};
	struct vdec_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->vdec = vdec;
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	vdec_fill_output(&pix);
	vdec_set_output(ctx, &pix);
	ret = vdec_init_ctrls(ctx);
	if (ret)
		goto err;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(vdec->m2m, ctx, vdec_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err;
	}
	v4l2_fh_add(&ctx->fh, file);
	return 0;

err:
	v4l2_ctrl_handler_free(&ctx->controls);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int vdec_release(struct file *file)
{
	struct vdec_ctx *ctx = file_to_ctx(file);

	mutex_lock(&ctx->vdec->queue_lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&ctx->vdec->queue_lock);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->controls);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations vdec_fops = {
	.owner = THIS_MODULE,
	.open = vdec_open,
	.release = vdec_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int vdec_hw_init(struct platform_device *pdev, struct vdec_dev *vdec)
{
	struct device *dev = &pdev->dev;
	struct clk *clk;
	u32 val;
	int ret;

	vdec->vdh = devm_platform_ioremap_resource_byname(pdev, "vdh");
	if (IS_ERR(vdec->vdh))
		return PTR_ERR(vdec->vdh);
	vdec->pxp = vdec->vdh + VDH_PXP0;
	vdec->global = devm_platform_ioremap_resource_byname(pdev, "vdh-global");
	if (IS_ERR(vdec->global))
		return PTR_ERR(vdec->global);
	vdec->scd = devm_platform_ioremap_resource_byname(pdev, "scd");
	if (IS_ERR(vdec->scd))
		return PTR_ERR(vdec->scd);
	vdec->smmu = devm_platform_ioremap_resource_byname(pdev, "smmu");
	if (IS_ERR(vdec->smmu))
		return PTR_ERR(vdec->smmu);

	vdec->pxp_irq = platform_get_irq_byname(pdev, "pxp");
	if (vdec->pxp_irq < 0)
		return vdec->pxp_irq;
	vdec->smmu_irq = platform_get_irq_byname(pdev, "smmu");
	if (vdec->smmu_irq < 0)
		return vdec->smmu_irq;

	vdec->iova_pool = devm_gen_pool_create(dev, ilog2(VDEC_PAGE), -1, NULL);
	if (IS_ERR(vdec->iova_pool))
		return PTR_ERR(vdec->iova_pool);
	ret = gen_pool_add(vdec->iova_pool, VDEC_IOVA_START, VDEC_IOVA_SIZE, -1);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	vdec->table = dmam_alloc_coherent(dev, VDEC_TABLE_SIZE, &vdec->table_dma, GFP_KERNEL);
	vdec->sinks = dmam_alloc_coherent(dev, VDEC_PAGE, &vdec->sinks_dma, GFP_KERNEL);
	if (!vdec->table || !vdec->sinks)
		return -ENOMEM;

	clk = devm_clk_get_enabled(dev, "vdec");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "could not enable VDEC CRG\n");

	val = readl(vdec->global + VDH_GLB_CLK_CTRL);
	writel((val & ~VDH_GLB_CLK_SEL_MASK) | VDH_GLB_CLK_SEL_FACTORY,
	       vdec->global + VDH_GLB_CLK_CTRL);
	readl(vdec->global + VDH_GLB_CLK_CTRL);

	ret = hi3798mv320_vpu_reset(dev, vdec->global, vdec->pxp);
	if (ret)
		return ret;
	writel(1, vdec->scd + SCD_IRQ_MASK);
	writel(1, vdec->scd + SCD_IRQ_CLEAR);
	writel(0xf, vdec->smmu + SMMU_IRQ_MASK);
	writel(vdec_pxp_ack(readl(vdec->pxp + VDH_IRQ_STATUS)), vdec->pxp + PXP_IRQ_CLEAR);

	ret = devm_request_irq(dev, vdec->pxp_irq, vdec_pxp_irq, 0, VDEC_NAME "-pxp", vdec);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, vdec->smmu_irq, vdec_smmu_irq, 0, VDEC_NAME "-smmu", vdec);
	if (ret)
		return ret;

	vdec_configure_smmu(vdec);
	return 0;
}

static int vdec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vdec_dev *vdec;
	int ret;

	vdec = devm_kzalloc(dev, sizeof(*vdec), GFP_KERNEL);
	if (!vdec)
		return -ENOMEM;
	vdec->dev = dev;
	mutex_init(&vdec->hw_lock);
	mutex_init(&vdec->queue_lock);
	init_completion(&vdec->done);
	INIT_WORK(&vdec->work, vdec_worker);

	ret = vdec_hw_init(pdev, vdec);
	if (ret)
		return ret;

	vdec->media.dev = dev;
	strscpy(vdec->media.model, VDEC_NAME, sizeof(vdec->media.model));
	vdec->media.ops = &vdec_media_ops;
	media_device_init(&vdec->media);
	vdec->v4l2.mdev = &vdec->media;
	ret = v4l2_device_register(dev, &vdec->v4l2);
	if (ret)
		goto cleanup_media;

	vdec->m2m = v4l2_m2m_init(&vdec_m2m_ops);
	if (IS_ERR(vdec->m2m)) {
		ret = PTR_ERR(vdec->m2m);
		goto unregister_v4l2;
	}

	strscpy(vdec->video.name, VDEC_NAME, sizeof(vdec->video.name));
	vdec->video.v4l2_dev = &vdec->v4l2;
	vdec->video.fops = &vdec_fops;
	vdec->video.ioctl_ops = &vdec_ioctl_ops;
	vdec->video.release = video_device_release_empty;
	vdec->video.lock = &vdec->queue_lock;
	vdec->video.vfl_dir = VFL_DIR_M2M;
	vdec->video.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(&vdec->video, vdec);
	ret = video_register_device(&vdec->video, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto release_m2m;

	ret = v4l2_m2m_register_media_controller(vdec->m2m, &vdec->video,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret)
		goto unregister_video;
	ret = media_device_register(&vdec->media);
	if (ret)
		goto unregister_controller;

	platform_set_drvdata(pdev, vdec);
	dev_info(dev, "H.264 stateless decoder registered as %s, PXP IRQ %d SMMU IRQ %d\n",
		 video_device_node_name(&vdec->video), vdec->pxp_irq, vdec->smmu_irq);
	return 0;

unregister_controller:
	v4l2_m2m_unregister_media_controller(vdec->m2m);
unregister_video:
	video_unregister_device(&vdec->video);
release_m2m:
	v4l2_m2m_release(vdec->m2m);
unregister_v4l2:
	v4l2_device_unregister(&vdec->v4l2);
cleanup_media:
	media_device_cleanup(&vdec->media);
	return ret;
}

static void vdec_remove(struct platform_device *pdev)
{
	struct vdec_dev *vdec = platform_get_drvdata(pdev);

	video_unregister_device(&vdec->video);
	media_device_unregister(&vdec->media);
	v4l2_m2m_unregister_media_controller(vdec->m2m);
	cancel_work_sync(&vdec->work);
	v4l2_m2m_release(vdec->m2m);
	v4l2_device_unregister(&vdec->v4l2);
	media_device_cleanup(&vdec->media);
	writel(0xf, vdec->smmu + SMMU_IRQ_MASK);
	writel(1, vdec->smmu + SMMU_CTRL);
}

static const struct of_device_id vdec_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-vdec" },
	{}
};
MODULE_DEVICE_TABLE(of, vdec_of_match);

static struct platform_driver vdec_driver = {
	.probe = vdec_probe,
	.remove = vdec_remove,
	.driver = {
		.name = VDEC_NAME,
		.of_match_table = vdec_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(vdec_driver);

MODULE_DESCRIPTION("HiSilicon Hi3798MV320 stateless H.264 and HEVC decoder");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
