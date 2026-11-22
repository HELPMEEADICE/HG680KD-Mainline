// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>
#include <media/media-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "hi3798mv320-vdec-cabac.h"
#include "hi3798mv320-vdh.h"

#define VDEC_NAME		"hi3798mv320-vdec"
#define VDEC_MIN_SIZE		32
#define VDEC_MAX_WIDTH		1920
#define VDEC_MAX_HEIGHT		1088 /* Coded height of a 1080-line H.264 picture. */
#define VDEC_MAX_STREAM		SZ_8M
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
#define SLICE_POOL		0x80000
#define SLICE_MSG_SIZE		0x400
#define VDEC_MAX_SLICES		136
#define STREAM_OFFSET		ALIGN(SLICE_POOL + (VDEC_MAX_SLICES - 1) * SLICE_MSG_SIZE, VDEC_PAGE)

struct vdec_dma {
	void *cpu;
	dma_addr_t dma;
	u32 iova;
	size_t size;
};

struct vdec_buffer {
	struct v4l2_m2m_buffer m2m;
	struct vdec_dma surface;
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
	u32 status, fault;
	unsigned int slot;
	bool tlb_dirty, dead;

	struct media_device media;
	struct v4l2_device v4l2;
	struct video_device video;
	struct v4l2_m2m_dev *m2m;
};

struct vdec_ctx {
	struct vdec_dev *vdec;
	struct v4l2_fh fh;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *sps, *pps, *scaling, *slice, *decode, *weights;
	struct v4l2_pix_format_mplane output, capture;
	struct vdec_dma workspace;
	unsigned int stride, y_size, vu_size, pmv_offset, pmv_size;
	unsigned int sequence_out, sequence_cap;
	unsigned int num_slices, stream_used, last_mb;
	struct vdec_buffer *held;
	u64 held_timestamp;
	struct v4l2_ctrl_h264_sps picture_sps;
	struct v4l2_ctrl_h264_pps picture_pps;
	struct v4l2_ctrl_h264_decode_params picture_dec;
};

static struct vdec_ctx *file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct vdec_ctx, fh);
}

static struct vdec_buffer *to_vdec_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct vdec_buffer, m2m.vb);
}

static int vdec_reset(struct vdec_dev *vdec);

/* The flat page table is private to the decoder and uses 4 KiB PTEs. */
static int vdec_dma_alloc(struct vdec_dev *vdec, struct vdec_dma *buf, size_t size)
{
	unsigned int pte, i;

	lockdep_assert_held(&vdec->hw_lock);
	if (vdec->dead)
		return -EIO;

	buf->size = ALIGN(size, VDEC_PAGE);
	buf->iova = gen_pool_alloc(vdec->iova_pool, buf->size);
	if (!buf->iova)
		return -ENOMEM;

	buf->cpu = dma_alloc_coherent(vdec->dev, buf->size, &buf->dma, GFP_KERNEL);
	if (!buf->cpu) {
		gen_pool_free(vdec->iova_pool, buf->iova, buf->size);
		return -ENOMEM;
	}
	if (!IS_ALIGNED(buf->dma, VDEC_PAGE)) {
		dma_free_coherent(vdec->dev, buf->size, buf->cpu, buf->dma);
		gen_pool_free(vdec->iova_pool, buf->iova, buf->size);
		buf->cpu = NULL;
		return -EFAULT;
	}

	pte = buf->iova / VDEC_PAGE;
	for (i = 0; i < buf->size / VDEC_PAGE; i++)
		vdec->table[pte + i] = cpu_to_le32((buf->dma + i * VDEC_PAGE) | 1);
	dma_wmb();
	dev_dbg(vdec->dev, "map IOVA %#x DMA %pad size %zu\n",
		buf->iova, &buf->dma, buf->size);
	return 0;
}

static void vdec_dma_free(struct vdec_dev *vdec, struct vdec_dma *buf)
{
	lockdep_assert_held(&vdec->hw_lock);
	if (!buf->cpu || vdec->dead)
		return;

	memset(vdec->table + buf->iova / VDEC_PAGE, 0,
	       buf->size / VDEC_PAGE * sizeof(*vdec->table));
	dma_wmb();

	/* Quiesce the walker and reload TTBR before handing out the IOVA again. */
	if (vdec->tlb_dirty && vdec_reset(vdec))
		return;
	dma_free_coherent(vdec->dev, buf->size, buf->cpu, buf->dma);
	gen_pool_free(vdec->iova_pool, buf->iova, buf->size);
	buf->cpu = NULL;
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

static int vdec_submit(struct vdec_dev *vdec, const struct vdec_dma *work)
{
	void __iomem *slot = vdec->vdh + VDH_SLOT + vdec->slot * VDH_SLOT_STRIDE;
	unsigned long done;
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

	if (ret) {
		const __le32 *up = work->cpu + MSG_UP;

		dev_err_ratelimited(vdec->dev,
				    "slot %u: decode %d PXP %#x SMMU %#x regs %08x/%08x/%08x UP %08x/%08x/%08x\n",
				    vdec->slot, ret, status, fault, readl(vdec->pxp + 0x20),
				    readl(vdec->pxp + 0x24), readl(vdec->pxp + 0x28),
				    le32_to_cpu(up[0]), le32_to_cpu(up[2]),
				    le32_to_cpu(up[0x80 / 4]));
		vdec_reset(vdec);
	} else if (vdec->slot == 1) {
		/* The hardware cursor advances past our two slots. Rewind it. */
		ret = vdec_reset(vdec);
	} else {
		vdec->slot = 1;
	}
	return ret;
}

static int vdec_find_refs(struct vdec_ctx *ctx, struct vdec_buffer *dst,
			  const struct v4l2_ctrl_h264_decode_params *dec,
			  struct vdec_refs *refs)
{
	struct vb2_queue *vq = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	unsigned int i, j, index = 0;

	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		if (!(dec->dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			continue;
		if (!(dec->dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID) ||
		    dec->dpb[i].fields != V4L2_H264_FRAME_REF)
			return -EINVAL;

		for (j = 0; j < vq->max_num_buffers; j++) {
			struct vb2_buffer *vb = vb2_get_buffer(vq, j);
			struct vdec_buffer *ref;

			if (!vb)
				continue;
			ref = to_vdec_buffer(vb);
			if (ref != dst && ref->decoded &&
			    ref->timestamp == dec->dpb[i].reference_ts) {
				refs->buffers[i] = ref;
				refs->index[i] = index++;
				break;
			}
		}
		if (!refs->buffers[i])
			return -ENOENT;
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
		if (!list)
			slice[36 + slot / 2] |= cpu_to_le32(i << (10 * (slot % 2)));
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
	slice[7] = cpu_to_le32(type | l0 << 2 | l1 << 8 | (l0 ? (l0 - 1) << 16 : 0) |
			      (l1 ? (l1 - 1) << 21 : 0) |
			      (sp->flags & V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED ?
			       BIT(15) : 0));
	if (l1) {
		struct vdec_buffer *col = refs->buffers[sp->ref_pic_list1[0].index];

		/* Progressive ColMV uses the frame PMV layout with bit 14 clear. */
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
	core[32] = vdec_addr(base, MSG_UP + 0x80);
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
	unsigned int row, col;

	for (row = 0; row < height; row++) {
		const u8 *band = src + row / tile_height * stride * tile_height;

		for (col = 0; col < width; col += 64) {
			unsigned int tile = col / 64;
			unsigned int mask = tile_height == 16 ? 3 : 1;
			unsigned int swizzle = (tile & mask) * 4;
			unsigned int y = (row % tile_height) ^ swizzle;

			memcpy(dst + row * width + col,
			       band + tile * 64 * tile_height + y * 64,
			       min(64U, width - col));
		}
	}
}

static int vdec_copy_rbsp(u8 *dst, const u8 *src, unsigned int size,
			  unsigned int prefix)
{
	unsigned int i, length = prefix + 1, zeroes = 0;

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
	return length;
}

static unsigned int vdec_slice_offset(unsigned int index)
{
	return index ? SLICE_POOL + (index - 1) * SLICE_MSG_SIZE : MSG_SLICE;
}

#define REQUEST_PICTURE		BIT(0)
#define REQUEST_SCALING		BIT(1)
#define REQUEST_WEIGHTS		BIT(2)

static int vdec_finish_picture(struct vdec_ctx *ctx, struct vb2_v4l2_buffer *dst)
{
	struct vdec_buffer *out = to_vdec_buffer(&dst->vb2_buf);
	unsigned int width = ctx->output.width, height = ctx->output.height;
	unsigned int mb_width = width / 16;
	__le32 *slice = ctx->workspace.cpu + vdec_slice_offset(ctx->num_slices - 1);
	u8 *output = vb2_plane_vaddr(&dst->vb2_buf, 0);
	int ret;

	vdec_slice_end(slice, mb_width * (height / 16) - 1, mb_width);
	ctx->num_slices = 0;
	if (ctx->vdec->dead)
		return -EIO;

	memset(out->surface.cpu, 0, out->surface.size);
	ret = vdec_submit(ctx->vdec, &ctx->workspace);
	if (ret)
		return ret;

	vdec_detile_plane(output, out->surface.cpu, width, height, ctx->stride, 16);
	vdec_detile_plane(output + width * height, out->surface.cpu + ctx->y_size,
			  width, height / 2, ctx->stride, 8);
	out->decoded = true;
	out->timestamp = ctx->held_timestamp;
	vb2_set_plane_payload(&dst->vb2_buf, 0, ctx->capture.plane_fmt[0].sizeimage);
	return 0;
}

static int vdec_run(struct vdec_ctx *ctx, struct vb2_v4l2_buffer *src,
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
	bool hold = src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
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

	ret = vdec_find_refs(ctx, out, dec, &refs);
	if (ret)
		return ret;
	if (!ctx->workspace.cpu) {
		ret = vdec_dma_alloc(ctx->vdec, &ctx->workspace,
				    STREAM_OFFSET + ctx->output.plane_fmt[0].sizeimage +
				    VDEC_STREAM_PAD);
		if (ret)
			return ret;
		memcpy(ctx->workspace.cpu + CABAC_TABLE, h264_cabac_init,
		       sizeof(h264_cabac_init));
	}
	if (!ctx->num_slices)
		vdec_picture_message(ctx, out, &refs);

	stream = ctx->workspace.cpu + STREAM_OFFSET + ctx->stream_used;
	/* The VDH entropy decoder consumes RBSP at the slice data offset. */
	ret = vdec_copy_rbsp(stream, input, bytes, prefix);
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
	if (hold)
		return 0;

	return vdec_finish_picture(ctx, dst);
}

static unsigned int vdec_request_controls(struct vdec_ctx *ctx, struct media_request *req)
{
	struct v4l2_ctrl_handler *hdl = v4l2_ctrl_request_hdl_find(req, &ctx->controls);
	unsigned int controls = 0;

	if (!hdl)
		return 0;
	if (v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_H264_PPS) &&
	    v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_H264_DECODE_PARAMS))
		controls |= REQUEST_PICTURE;
	if (v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_H264_SCALING_MATRIX))
		controls |= REQUEST_SCALING;
	if (v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_H264_PRED_WEIGHTS))
		controls |= REQUEST_WEIGHTS;
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
		ret = vdec_run(ctx, src, dst, controls);
		mutex_unlock(ctx->controls.lock);
	}
	if (ret) {
		ctx->num_slices = 0;
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

	if (*num_planes)
		return *num_planes == 1 && sizes[0] >= size ? 0 : -EINVAL;
	*num_planes = 1;
	sizes[0] = size;
	return 0;
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
	ret = vdec_dma_alloc(ctx->vdec, &buf->surface, ctx->pmv_offset + ctx->pmv_size);
	mutex_unlock(&ctx->vdec->hw_lock);
	return ret;
}

static void vdec_buf_cleanup(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	mutex_lock(&ctx->vdec->hw_lock);
	vdec_dma_free(ctx->vdec, &to_vdec_buffer(vb)->surface);
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
	src->mem_ops = &vb2_dma_contig_memops;
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
	dst->mem_ops = &vb2_dma_contig_memops;
	dst->buf_struct_size = sizeof(struct vdec_buffer);
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &ctx->vdec->queue_lock;
	dst->dev = ctx->vdec->dev;
	return vb2_queue_init(dst);
}

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
	if (!v4l2_ctrl_request_hdl_ctrl_find(hdl, V4L2_CID_STATELESS_H264_SLICE_PARAMS))
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

	pix->width = ALIGN(clamp(pix->width, VDEC_MIN_SIZE, VDEC_MAX_WIDTH), 16);
	pix->height = ALIGN(clamp(pix->height, VDEC_MIN_SIZE, VDEC_MAX_HEIGHT), 16);
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
			      struct v4l2_pix_format_mplane *cap)
{
	*cap = *out;
	cap->pixelformat = V4L2_PIX_FMT_NV21;
	cap->plane_fmt[0].bytesperline = cap->width;
	cap->plane_fmt[0].sizeimage = cap->width * cap->height * 3 / 2;
}

static void vdec_set_output(struct vdec_ctx *ctx, struct v4l2_pix_format_mplane *pix)
{
	ctx->output = *pix;
	vdec_fill_capture(&ctx->output, &ctx->capture);
	ctx->stride = ALIGN(pix->width, 256);
	ctx->y_size = ctx->stride * ALIGN(pix->height, 64);
	ctx->vu_size = ctx->y_size / 2;
	ctx->pmv_offset = ALIGN(ctx->y_size + ctx->vu_size + 1024, VDEC_PAGE);
	ctx->pmv_size = pix->width * pix->height / 4 + 256;
}

static int vdec_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_TYPE_IS_OUTPUT(f->type) ?
			 V4L2_PIX_FMT_H264_SLICE : V4L2_PIX_FMT_NV21;
	return 0;
}

static int vdec_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *f)
{
	if (f->index || (f->pixel_format != V4L2_PIX_FMT_H264_SLICE &&
			 f->pixel_format != V4L2_PIX_FMT_NV21))
		return -EINVAL;
	f->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	f->stepwise.min_width = VDEC_MIN_SIZE;
	f->stepwise.max_width = VDEC_MAX_WIDTH;
	f->stepwise.step_width = 16;
	f->stepwise.min_height = VDEC_MIN_SIZE;
	f->stepwise.max_height = VDEC_MAX_HEIGHT;
	f->stepwise.step_height = 16;
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
	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		vdec_fill_output(&f->fmt.pix_mp);
		return 0;
	}
	return vdec_g_fmt(file, priv, f);
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
	if (!V4L2_TYPE_IS_OUTPUT(f->type))
		return 0;
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

MODULE_DESCRIPTION("HiSilicon Hi3798MV320 stateless H.264 decoder");
MODULE_LICENSE("GPL");
