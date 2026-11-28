// SPDX-License-Identifier: GPL-2.0-only
/* Hi3798MV320 V500 R002 VEDU H.264 encoder */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/unaligned.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define VENC_NAME		"hi3798mv320-venc"
#define VENC_MIN_WIDTH		256
#define VENC_MIN_HEIGHT		128
#define VENC_MAX_WIDTH		1920
#define VENC_MAX_HEIGHT		1088
#define VENC_ALIGN		64
#define VENC_GUARD		0xa5
#define VENC_SMMU_OFFSET	0xf000
#define VENC_WINDOW_SIZE	0x10000

#define VEDU_IRQ_ENABLE		0x000
#define VEDU_IRQ_CLEAR		0x004
#define VEDU_START		0x008
#define VEDU_DRAIN		0x014
#define VEDU_STATUS		0x3000
#define VEDU_ERROR		0x3004
#define VEDU_BUS_STATE		0x3010
#define VEDU_PADDED_BYTES	0x3844
#define VEDU_TAIL		0x3848
#define VEDU_EOF		BIT(0)
#define VEDU_SLICE		BIT(1)
#define VEDU_TIMEOUT		BIT(6)
#define VEDU_OVERFLOW		(BIT(2) | BIT(3))
#define VEDU_END_OF_SLICE	BIT(31)
#define VEDU_LENGTH_MASK	GENMASK(28, 0)
#define VEDU_PADDING_MASK	GENMASK(6, 0)

enum venc_area {
	VENC_SOURCE, VENC_WORK, VENC_PARAMS, VENC_POINTERS, VENC_STREAM,
	VENC_ERR_READ, VENC_ERR_WRITE, VENC_NUM_AREAS,
};

struct venc_dma {
	u8 *cpu;
	dma_addr_t dma;
	size_t size, length;
};

struct venc_layout {
	u32 src_y, rec_y, rec_size, stream_capacity;
	u32 t_offset, m_offset, p_offset, a_offset, b_offset, c_offset;
	u32 aux_t, aux_m, aux_p;
};

struct venc_event {
	u32 status, error, length, tail, fault_s, fault_ns;
};

struct venc_dev {
	struct device *dev;
	void __iomem *core, *smmu;
	struct clk *clk;
	struct reset_control *core_reset, *smmu_reset;
	int irq;
	struct mutex lock;
	wait_queue_head_t wait;
	bool opened, finished, quarantined;
	struct venc_event event;
	struct venc_dma area[VENC_NUM_AREAS];
	atomic64_t drained, faults;
	struct v4l2_device v4l2;
	struct video_device video;
	struct v4l2_m2m_dev *m2m;
};

struct venc_ctx {
	struct v4l2_fh fh;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *qp;
	struct venc_dev *venc;
	struct v4l2_pix_format_mplane output, capture;
	struct venc_layout layout;
	struct work_struct work;
	bool aborted;
	u32 output_sequence, capture_sequence;
};

struct venc_reg {
	u16 offset;
	u32 value;
};

struct venc_bits {
	u8 data[24];
	unsigned int count;
};

#include "hi3798mv320-venc-r002.h"

static struct venc_ctx *venc_file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct venc_ctx, fh);
}

static void venc_put_bits(struct venc_bits *bits, u32 value, unsigned int count)
{
	while (count--) {
		unsigned int pos = bits->count++;

		bits->data[pos / 8] |= ((value >> count) & 1) << (7 - pos % 8);
	}
}

static void venc_put_ue(struct venc_bits *bits, unsigned int value)
{
	unsigned int width = fls(value + 1);

	venc_put_bits(bits, 0, width - 1);
	venc_put_bits(bits, value + 1, width);
}

static size_t venc_append_nal(u8 *dst, u8 type, struct venc_bits *rbsp)
{
	unsigned int zeros = 0, i;
	size_t used = 5;

	put_unaligned_be32(1, dst);
	dst[4] = type;
	venc_put_bits(rbsp, 1, 1);
	for (i = 0; i < DIV_ROUND_UP(rbsp->count, 8); i++) {
		u8 byte = rbsp->data[i];

		if (zeros == 2 && byte <= 3) {
			dst[used++] = 3;
			zeros = 0;
		}
		dst[used++] = byte;
		zeros = byte ? 0 : zeros + 1;
	}
	return used;
}

static size_t venc_headers(struct venc_ctx *ctx, u8 *params, u32 *slice, u32 *lengths)
{
	struct venc_bits sps = {}, pps = {}, hdr = {};
	u32 width = ctx->output.width, height = ctx->output.height;
	u32 right = (ALIGN(width, 16) - width) / 2;
	u32 bottom = (ALIGN(height, 16) - height) / 2;
	size_t used;

	venc_put_bits(&sps, 66, 8);	/* Baseline */
	venc_put_bits(&sps, 0xc0, 8);	/* Constrained Baseline */
	venc_put_bits(&sps, 42, 8);	/* Level 4.2 */
	venc_put_ue(&sps, 0);		/* SPS id */
	venc_put_ue(&sps, 0);		/* 4-bit frame_num */
	venc_put_ue(&sps, 2);		/* POC type 2 */
	venc_put_ue(&sps, 1);		/* max_num_ref_frames */
	venc_put_bits(&sps, 0, 1);	/* No frame_num gaps */
	venc_put_ue(&sps, DIV_ROUND_UP(width, 16) - 1);
	venc_put_ue(&sps, DIV_ROUND_UP(height, 16) - 1);
	venc_put_bits(&sps, 3, 2);	/* Progressive and direct_8x8_inference */
	venc_put_bits(&sps, !!(right || bottom), 1);
	if (right || bottom) {
		venc_put_ue(&sps, 0);
		venc_put_ue(&sps, right);
		venc_put_ue(&sps, 0);
		venc_put_ue(&sps, bottom);
	}
	venc_put_bits(&sps, 0, 1);	/* No VUI */
	used = venc_append_nal(params, 0x67, &sps);

	venc_put_ue(&pps, 0);		/* PPS id */
	venc_put_ue(&pps, 0);		/* SPS id */
	venc_put_bits(&pps, 0, 2);	/* CAVLC, no bottom-field POC */
	venc_put_ue(&pps, 0);		/* One slice group */
	venc_put_ue(&pps, 0);
	venc_put_ue(&pps, 0);		/* Default reference list sizes */
	venc_put_bits(&pps, 0, 3);	/* No weighted prediction */
	venc_put_ue(&pps, 0);		/* Initial QP 26 */
	venc_put_ue(&pps, 0);		/* Initial QS 26 */
	venc_put_ue(&pps, 0);		/* Chroma QP offset */
	venc_put_bits(&pps, 4, 3);	/* Deblocking fields present */

	/* VEDU inserts first_mb_in_slice, the QP delta and deblocking fields. */
	venc_put_ue(&hdr, 2);		/* I slice */
	venc_put_ue(&hdr, 0);		/* PPS id */
	venc_put_bits(&hdr, 0, 4);	/* frame_num */
	venc_put_ue(&hdr, ctx->output_sequence & 1); /* Alternate idr_pic_id. */
	*slice = get_unaligned_be32(hdr.data) >> (32 - hdr.count);
	/* One zero reorder bit and two zero IDR marking bits. */
	*lengths = FIELD_PREP(GENMASK(22, 16), hdr.count - 1) | 1;

	return used + venc_append_nal(params + used, 0x68, &pps);
}

static u8 *venc_payload(struct venc_dev *v, enum venc_area id)
{
	return v->area[id].cpu + PAGE_SIZE;
}

static u32 venc_addr(struct venc_dev *v, enum venc_area id, u32 offset)
{
	return v->area[id].dma + PAGE_SIZE + offset;
}

static void venc_free_areas(struct venc_dev *v)
{
	unsigned int i;

	if (READ_ONCE(v->quarantined))
		return;
	for (i = 0; i < VENC_NUM_AREAS; i++) {
		struct venc_dma *b = &v->area[i];

		if (b->cpu)
			dma_free_coherent(v->dev, b->size, b->cpu, b->dma);
		*b = (struct venc_dma){};
	}
}

static int venc_alloc_areas(struct venc_ctx *ctx)
{
	struct venc_dev *v = ctx->venc;
	struct venc_layout *l = &ctx->layout;
	u32 width = ctx->output.width, height = ctx->output.height;
	u32 stride = ctx->output.plane_fmt[0].bytesperline;
	bool hd = width > 1280 || height > 720;
	u32 d = hd ? 1920 : 1280;
	u32 cols = DIV_ROUND_UP(d, 32), mb_cols = DIV_ROUND_UP(d, 16);
	size_t length[VENC_NUM_AREAS];
	unsigned int i;
	int ret;

	if (READ_ONCE(v->quarantined))
		return -EIO;
	if (v->area[VENC_SOURCE].cpu)
		return 0;

	l->src_y = stride * ALIGN(height, 16);
	l->rec_y = hd ? 0x1fe000 : 0xe1000;
	l->rec_size = l->rec_y * 3 / 2;
	l->aux_t = 128 * cols * DIV_ROUND_UP(d, 256);
	l->aux_m = 128 * cols * cols;
	l->aux_p = 32 * (mb_cols + 1);
	l->t_offset = 2 * l->rec_size;
	l->m_offset = l->t_offset + 2 * l->aux_t;
	l->p_offset = l->m_offset + 2 * l->aux_m;
	l->a_offset = l->p_offset + 3 * l->aux_p;
	l->b_offset = l->a_offset + 256 * ALIGN(d, 16);
	l->c_offset = l->b_offset + mb_cols * ALIGN(d, 256) / 32;
	l->stream_capacity = ctx->capture.plane_fmt[0].sizeimage;

	length[VENC_SOURCE] = l->src_y * 3 / 2;
	length[VENC_WORK] = l->c_offset + mb_cols * ALIGN(d, 128) / 16;
	length[VENC_PARAMS] = PAGE_SIZE;
	length[VENC_POINTERS] = PAGE_SIZE;
	length[VENC_STREAM] = VENC_ALIGN + l->stream_capacity;
	length[VENC_ERR_READ] = PAGE_SIZE;
	length[VENC_ERR_WRITE] = PAGE_SIZE;

	for (i = 0; i < VENC_NUM_AREAS; i++) {
		struct venc_dma *b = &v->area[i];

		b->length = length[i];
		b->size = PAGE_ALIGN(length[i]) + 2 * PAGE_SIZE;
		b->cpu = dma_alloc_coherent(v->dev, b->size, &b->dma, GFP_KERNEL);
		if (!b->cpu) {
			ret = -ENOMEM;
			goto fail;
		}
		if (!b->dma || b->dma + b->size - 1 > U32_MAX ||
		    !IS_ALIGNED(b->dma, PAGE_SIZE)) {
			ret = -ERANGE;
			goto fail;
		}
		memset(b->cpu, VENC_GUARD, PAGE_SIZE);
		memset(b->cpu + PAGE_SIZE + b->length, VENC_GUARD,
		       b->size - PAGE_SIZE - b->length);
	}
	memset(venc_payload(v, VENC_STREAM), VENC_GUARD, VENC_ALIGN);
	return 0;

fail:
	venc_free_areas(v);
	return ret;
}

static bool venc_guards_ok(struct venc_dev *v)
{
	unsigned int i;

	for (i = 0; i < VENC_NUM_AREAS; i++) {
		struct venc_dma *b = &v->area[i];
		size_t end = PAGE_SIZE + b->length;

		if (memchr_inv(b->cpu, VENC_GUARD, PAGE_SIZE) ||
		    memchr_inv(b->cpu + end, VENC_GUARD, b->size - end)) {
			dev_err(v->dev, "DMA guard changed in area %u\n", i);
			return false;
		}
	}
	return !memchr_inv(venc_payload(v, VENC_STREAM), VENC_GUARD, VENC_ALIGN);
}

static void venc_copy_source(struct venc_ctx *ctx, const u8 *src)
{
	struct venc_dev *v = ctx->venc;
	u32 stride = ctx->output.plane_fmt[0].bytesperline;
	u32 width = ctx->output.width, height = ctx->output.height;
	u32 rows = ALIGN(height, 16), y, x;
	u8 *dst = venc_payload(v, VENC_SOURCE);

	/* Replicate edge samples to the DMA stride and macroblock height. */
	for (y = 0; y < rows; y++, dst += stride) {
		const u8 *row = src + min(y, height - 1) * stride;

		memcpy(dst, row, width);
		memset(dst + width, row[width - 1], stride - width);
	}
	src += height * stride;
	for (y = 0; y < rows / 2; y++, dst += stride) {
		const u8 *row = src + min(y, height / 2 - 1) * stride;

		memcpy(dst, row, width);
		for (x = width; x < stride; x += 2) {
			dst[x] = row[width - 2];
			dst[x + 1] = row[width - 1];
		}
	}
}

static void venc_program_core(struct venc_ctx *ctx, u32 qp, u32 slice, u32 lengths)
{
	struct venc_dev *v = ctx->venc;
	const struct venc_layout *l = &ctx->layout;
	const u32 w = ctx->output.width, h = ctx->output.height;
	const u32 stride = ctx->output.plane_fmt[0].bytesperline;
	const u32 strides = (stride << 16) | stride;
	const u32 ref = venc_addr(v, VENC_WORK, 0);
	const u32 rec = venc_addr(v, VENC_WORK, l->rec_size);
	const u8 *headers = venc_payload(v, VENC_PARAMS);
	const struct venc_reg dynamic[] = {
		{ 0x044, ((h - 1) << 16) | (w - 1) },
		{ 0x048, 0x80000 | FIELD_PREP(GENMASK(15, 10), qp) },
		{ 0x05c, BIT(16) | (qp << 8) | qp },
		{ 0x104, venc_addr(v, VENC_SOURCE, 0) },
		{ 0x108, venc_addr(v, VENC_SOURCE, l->src_y) },
		{ 0x11c, ref + l->m_offset }, { 0x120, ref + l->m_offset },
		{ 0x124, ref + l->m_offset + l->aux_m },
		{ 0x128, ref + l->m_offset + l->aux_m },
		{ 0x12c, venc_addr(v, VENC_POINTERS, 16) },
		{ 0x130, venc_addr(v, VENC_POINTERS, 0) },
		{ 0x134, ref + l->p_offset + l->aux_p }, { 0x138, ref + l->p_offset },
		{ 0x13c, ref + l->c_offset }, { 0x140, ref + l->b_offset },
		{ 0x144, ref + l->a_offset },
		{ 0x148, stride * h }, { 0x14c, stride * h / 2 },
		{ 0x164, ref + l->t_offset + l->aux_t }, { 0x168, ref + l->t_offset },
		{ 0x16c, strides }, { 0x170, strides }, { 0x160, strides },
		{ 0x174, ref }, { 0x178, ref + l->rec_y },
		{ 0x17c, ref }, { 0x180, ref + l->rec_y },
		{ 0x184, ref }, { 0x188, ref + l->rec_y },
		{ 0x18c, ref }, { 0x190, ref + l->rec_y },
		{ 0x194, rec }, { 0x198, rec + l->rec_y },
		{ 0x19c, rec }, { 0x1a0, rec + l->rec_y },
		{ 0x1f0, strides },
		{ 0x23c, ctx->output.pixelformat == V4L2_PIX_FMT_NV21 ? BIT(8) : 0 },
		{ 0x27c, l->stream_capacity },
		{ 0x2bc, venc_addr(v, VENC_STREAM, VENC_ALIGN) },
		{ 0x2fc, venc_addr(v, VENC_PARAMS, 0) },
		{ 0x454, slice }, { 0x474, lengths },
	};
	unsigned int i, j;

	/* Both writes to the RP/WP registers must use the same DMA addresses. */
	for (i = 0; i < ARRAY_SIZE(r002_idr); i++) {
		u16 reg = r002_idr[i].offset;
		u32 value = r002_idr[i].value;

		for (j = 0; j < ARRAY_SIZE(dynamic); j++)
			if (dynamic[j].offset == reg) {
				value = dynamic[j].value;
				break;
			}
		if (reg >= 0x304 && reg < 0x304 + VENC_ALIGN)
			value = get_unaligned_le32(headers + reg - 0x304);
		writel_relaxed(value, v->core + reg);
	}
}

static void venc_ack(struct venc_dev *v)
{
	writel(U32_MAX, v->core + VEDU_IRQ_CLEAR);
	writel(U32_MAX, v->smmu + 0x01c);
	writel(U32_MAX, v->smmu + 0x02c);
	readl(v->core + VEDU_STATUS);
}

static struct venc_event venc_snapshot(struct venc_dev *v)
{
	return (struct venc_event) {
		.status = readl(v->core + VEDU_STATUS),
		.error = readl(v->core + VEDU_ERROR),
		.length = readl(v->core + VEDU_PADDED_BYTES),
		.tail = readl(v->core + VEDU_TAIL),
		.fault_s = readl(v->smmu + 0x018) & 0xf,
		.fault_ns = readl(v->smmu + 0x028) & 0xf,
	};
}

static irqreturn_t venc_irq(int irq, void *data)
{
	struct venc_dev *v = data;
	struct venc_event e = venc_snapshot(v);
	bool terminal = (e.status & (VEDU_EOF | VEDU_TIMEOUT)) ||
			(e.error & VEDU_OVERFLOW) || e.fault_s || e.fault_ns;

	if (!terminal && !(e.status & VEDU_SLICE))
		return IRQ_NONE;
	if (terminal) {
		v->event = e;
		writel(0, v->core + VEDU_IRQ_ENABLE);
	}
	venc_ack(v);
	if (terminal) {
		WRITE_ONCE(v->finished, true);
		wake_up(&v->wait);
	}
	return IRQ_HANDLED;
}

static int venc_reset_domain(struct reset_control *rst)
{
	int ret = reset_control_assert(rst);

	if (ret)
		return ret;
	udelay(5);
	ret = reset_control_deassert(rst);
	udelay(10);
	return ret;
}

static int venc_reset(struct venc_dev *v)
{
	int ret = venc_reset_domain(v->smmu_reset);

	return ret ?: venc_reset_domain(v->core_reset);
}

static int venc_drain(struct venc_dev *v)
{
	u32 before, state;
	int ret;

	before = readl(v->core + VEDU_BUS_STATE);
	writel(1, v->core + VEDU_DRAIN);
	ret = readl_poll_timeout(v->core + VEDU_BUS_STATE, state, state, 10, 1000000);
	writel(0, v->core + VEDU_DRAIN);
	readl(v->core + VEDU_DRAIN);
	if (!ret && before)
		ret = -EIO;
	if (ret)
		dev_err(v->dev, "bus drain failed: %#x -> %#x (%d)\n", before, state, ret);
	return ret;
}

static int venc_run(struct venc_ctx *ctx, const u8 *source, u8 *dest, size_t dest_size)
{
	struct venc_dev *v = ctx->venc;
	struct venc_event before, after;
	struct venc_event *e = &v->event;
	u8 params[VENC_ALIGN] = {};
	const u8 *stream = venc_payload(v, VENC_STREAM) + VENC_ALIGN;
	u32 slice, lengths, bytes, padded, padding;
	size_t headers;
	int ret, reset;

	if (READ_ONCE(v->quarantined))
		return -EIO;
	if (READ_ONCE(ctx->aborted))
		return -ECANCELED;
	venc_copy_source(ctx, source);
	headers = venc_headers(ctx, params, &slice, &lengths);
	memset(venc_payload(v, VENC_PARAMS), 0, PAGE_SIZE);
	memcpy(venc_payload(v, VENC_PARAMS), params, sizeof(params));
	memset(venc_payload(v, VENC_POINTERS), 0, PAGE_SIZE);

	ret = clk_prepare_enable(v->clk);
	if (ret)
		return ret;
	ret = venc_reset(v);
	if (ret)
		goto disable_clk;
	writel(0, v->core + VEDU_IRQ_ENABLE);
	venc_program_core(ctx, v4l2_ctrl_g_ctrl(ctx->qp), slice, lengths);

	/* All VEDU masters use 32-bit physical DMA addresses in global bypass. */
	writel(0x9, v->smmu + 0x000);
	writel(0, v->smmu + 0x008);
	writel(0, v->smmu + 0x010);
	writel(0, v->smmu + 0x020);
	writel(0, v->smmu + 0x530);
	writel(0, v->smmu + 0x2ec);
	writel(venc_addr(v, VENC_ERR_READ, 0), v->smmu + 0x304);
	writel(venc_addr(v, VENC_ERR_WRITE, 0), v->smmu + 0x30c);
	venc_ack(v);
	if (readl(v->core + VEDU_BUS_STATE) || readl(v->smmu) != 0x9 ||
	    readl(v->smmu + 0x2ec) ||
	    readl(v->core + 0x104) != venc_addr(v, VENC_SOURCE, 0) ||
	    readl(v->core + 0x108) != venc_addr(v, VENC_SOURCE, ctx->layout.src_y) ||
	    readl(v->core + 0x12c) != venc_addr(v, VENC_POINTERS, 16) ||
	    readl(v->core + 0x130) != venc_addr(v, VENC_POINTERS, 0) ||
	    readl(v->core + 0x2bc) != venc_addr(v, VENC_STREAM, VENC_ALIGN) ||
	    readl(v->core + 0x27c) != ctx->layout.stream_capacity) {
		dev_err(v->dev, "unsafe VEDU address or bus state before START\n");
		ret = -EIO;
		goto disable_clk;
	}
	if (READ_ONCE(ctx->aborted)) {
		ret = -ECANCELED;
		goto disable_clk;
	}

	*e = (struct venc_event){};
	WRITE_ONCE(v->finished, false);
	enable_irq(v->irq);
	writel(VEDU_EOF | VEDU_TIMEOUT, v->core + VEDU_IRQ_ENABLE);
	dma_wmb();
	writel(1, v->core + VEDU_START);
	ret = wait_event_timeout(v->wait, READ_ONCE(v->finished) || READ_ONCE(ctx->aborted),
				 msecs_to_jiffies(500)) ? 0 : -ETIMEDOUT;
	writel(0, v->core + VEDU_IRQ_ENABLE);
	disable_irq(v->irq);

	before = venc_snapshot(v);
	reset = venc_drain(v);
	after = venc_snapshot(v);
	if (e->fault_s || e->fault_ns || before.fault_s || before.fault_ns ||
	    after.fault_s || after.fault_ns) {
		atomic64_inc(&v->faults);
		dev_err(v->dev, "VENC SMMU fault S=%#x NS=%#x\n",
			e->fault_s | before.fault_s | after.fault_s,
			e->fault_ns | before.fault_ns | after.fault_ns);
		ret = -EIO;
	}
	if (!reset) {
		venc_ack(v);
		reset = venc_reset(v);
		if (!reset && readl(v->core + VEDU_BUS_STATE))
			reset = -EIO;
	}
	if (reset) {
		WRITE_ONCE(v->quarantined, true);
		dev_err(v->dev, "isolating DMA and keeping VENC gates enabled until reboot (%d)\n",
			reset);
		return reset;
	}
	atomic64_inc(&v->drained);
	if (READ_ONCE(ctx->aborted)) {
		ret = -ECANCELED;
		goto disable_clk;
	}

	padded = FIELD_GET(VEDU_LENGTH_MASK, e->length);
	padding = FIELD_GET(VEDU_PADDING_MASK, e->tail);
	if (ret || !(e->status & VEDU_EOF) ||
	    ((e->status | before.status | after.status) & VEDU_TIMEOUT) ||
	    ((e->error | before.error | after.error) & VEDU_OVERFLOW) ||
	    !(e->tail & VEDU_END_OF_SLICE) || padded <= padding ||
	    padded > ctx->layout.stream_capacity) {
		dev_err_ratelimited(v->dev, "encode failed: status=%#x error=%#x length=%#x tail=%#x (%d)\n",
				    e->status | before.status | after.status,
				    e->error | before.error | after.error, e->length, e->tail, ret);
		ret = ret ?: -EIO;
		goto disable_clk;
	}
	bytes = padded - padding;
	dma_rmb();
	if (bytes < 5 || bytes + headers > dest_size || !venc_guards_ok(v) ||
	    get_unaligned_be32(stream) != 1 || (stream[4] & 0x1f) != 5) {
		dev_err_ratelimited(v->dev, "invalid VENC DMA output (%u bytes)\n", bytes);
		ret = -EIO;
		goto disable_clk;
	}
	memcpy(dest, params, headers);
	memcpy(dest + headers, stream, bytes);
	ret = headers + bytes;

disable_clk:
	clk_disable_unprepare(v->clk);
	return ret;
}

static struct v4l2_pix_format_mplane *venc_format(struct venc_ctx *ctx, unsigned int type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? &ctx->output : &ctx->capture;
}

static void venc_raw_format(struct v4l2_pix_format_mplane *pix)
{
	u32 stride = ALIGN(clamp(pix->width, VENC_MIN_WIDTH, VENC_MAX_WIDTH), VENC_ALIGN);
	u32 fourcc = pix->pixelformat, width = pix->width, height = pix->height;

	*pix = (struct v4l2_pix_format_mplane) {
		.width = ALIGN(clamp(width, VENC_MIN_WIDTH, VENC_MAX_WIDTH), 2),
		.height = ALIGN(clamp(height, VENC_MIN_HEIGHT, VENC_MAX_HEIGHT), 2),
		.pixelformat = fourcc == V4L2_PIX_FMT_NV21 ? fourcc : V4L2_PIX_FMT_NV12,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_REC709,
		.num_planes = 1,
	};
	pix->plane_fmt[0].bytesperline = stride;
	pix->plane_fmt[0].sizeimage = stride * pix->height * 3 / 2;
}

static void venc_coded_format(struct v4l2_pix_format_mplane *pix,
			      const struct v4l2_pix_format_mplane *raw)
{
	*pix = *raw;
	pix->pixelformat = V4L2_PIX_FMT_H264;
	pix->plane_fmt[0].bytesperline = 0;
	pix->plane_fmt[0].sizeimage = PAGE_ALIGN(4 * raw->width * raw->height + 4096);
}

static int venc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct venc_dev *v = venc_file_to_ctx(file)->venc;

	strscpy(cap->driver, "hi3798-venc", sizeof(cap->driver));
	strscpy(cap->card, "Hi3798MV320 H.264 encoder", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", dev_name(v->dev));
	return 0;
}

static int venc_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	static const u32 raw[] = { V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV21 };

	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		if (f->index >= ARRAY_SIZE(raw))
			return -EINVAL;
		f->pixelformat = raw[f->index];
	} else {
		if (f->index)
			return -EINVAL;
		f->pixelformat = V4L2_PIX_FMT_H264;
		f->flags = V4L2_FMT_FLAG_COMPRESSED;
	}
	return 0;
}

static int venc_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *f)
{
	if (f->index || (f->pixel_format != V4L2_PIX_FMT_NV12 &&
			 f->pixel_format != V4L2_PIX_FMT_NV21 &&
			 f->pixel_format != V4L2_PIX_FMT_H264))
		return -EINVAL;
	f->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	f->stepwise = (struct v4l2_frmsize_stepwise) {
		.min_width = VENC_MIN_WIDTH, .max_width = VENC_MAX_WIDTH, .step_width = 2,
		.min_height = VENC_MIN_HEIGHT, .max_height = VENC_MAX_HEIGHT, .step_height = 2,
	};
	return 0;
}

static int venc_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct venc_ctx *ctx = venc_file_to_ctx(file);

	if (V4L2_TYPE_IS_OUTPUT(f->type))
		venc_raw_format(&f->fmt.pix_mp);
	else
		venc_coded_format(&f->fmt.pix_mp, &ctx->output);
	return 0;
}

static int venc_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = *venc_format(venc_file_to_ctx(file), f->type);
	return 0;
}

static int venc_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct venc_ctx *ctx = venc_file_to_ctx(file);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;

	if (vb2_is_busy(v4l2_m2m_get_vq(m2m, f->type)) ||
	    (V4L2_TYPE_IS_OUTPUT(f->type) && vb2_is_busy(v4l2_m2m_get_dst_vq(m2m))))
		return -EBUSY;
	venc_try_fmt(file, priv, f);
	*venc_format(ctx, f->type) = f->fmt.pix_mp;
	if (V4L2_TYPE_IS_OUTPUT(f->type))
		venc_coded_format(&ctx->capture, &ctx->output);
	return 0;
}

static int venc_log_status(struct file *file, void *priv)
{
	struct venc_dev *v = venc_file_to_ctx(file)->venc;

	v4l2_info(&v->v4l2, "drained=%lld SMMU faults=%lld quarantined=%u\n",
		  atomic64_read(&v->drained), atomic64_read(&v->faults),
		  READ_ONCE(v->quarantined));
	return 0;
}

static const struct v4l2_ioctl_ops venc_ioctls = {
	.vidioc_querycap = venc_querycap,
	.vidioc_enum_fmt_vid_out = venc_enum_fmt,
	.vidioc_enum_fmt_vid_cap = venc_enum_fmt,
	.vidioc_enum_framesizes = venc_enum_framesizes,
	.vidioc_try_fmt_vid_out_mplane = venc_try_fmt,
	.vidioc_try_fmt_vid_cap_mplane = venc_try_fmt,
	.vidioc_g_fmt_vid_out_mplane = venc_g_fmt,
	.vidioc_g_fmt_vid_cap_mplane = venc_g_fmt,
	.vidioc_s_fmt_vid_out_mplane = venc_s_fmt,
	.vidioc_s_fmt_vid_cap_mplane = venc_s_fmt,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = v4l2_m2m_ioctl_encoder_cmd,
	.vidioc_log_status = venc_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int venc_queue_setup(struct vb2_queue *q, unsigned int *buffers,
			    unsigned int *planes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct venc_ctx *ctx = vb2_get_drv_priv(q);
	u32 size = venc_format(ctx, q->type)->plane_fmt[0].sizeimage;

	if (*planes)
		return *planes == 1 && sizes[0] >= size ? 0 : -EINVAL;
	*planes = 1;
	sizes[0] = size;
	return 0;
}

static int venc_buf_prepare(struct vb2_buffer *vb)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	u32 size = venc_format(ctx, vb->type)->plane_fmt[0].sizeimage;

	if (vb2_plane_size(vb, 0) < size || vb->planes[0].data_offset)
		return -EINVAL;
	if (V4L2_TYPE_IS_OUTPUT(vb->type)) {
		if (vb2_get_plane_payload(vb, 0) < size)
			return -EINVAL;
		to_vb2_v4l2_buffer(vb)->field = V4L2_FIELD_NONE;
	} else {
		vb2_set_plane_payload(vb, 0, 0);
	}
	return 0;
}

static void venc_buf_queue(struct vb2_buffer *vb)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *buf = to_vb2_v4l2_buffer(vb);

	if (!V4L2_TYPE_IS_OUTPUT(vb->type) && v4l2_m2m_dst_buf_is_last(m2m)) {
		buf->sequence = ctx->capture_sequence++;
		v4l2_m2m_last_buffer_done(m2m, buf);
	} else {
		v4l2_m2m_buf_queue(m2m, buf);
	}
}

static void venc_return_buffers(struct vb2_queue *q, enum vb2_buffer_state state)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(q);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *buf;

	while ((buf = V4L2_TYPE_IS_OUTPUT(q->type) ?
		      v4l2_m2m_src_buf_remove(m2m) : v4l2_m2m_dst_buf_remove(m2m)))
		v4l2_m2m_buf_done(buf, state);
}

static int venc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(q);
	int ret = venc_alloc_areas(ctx);

	if (ret) {
		venc_return_buffers(q, VB2_BUF_STATE_QUEUED);
		return ret;
	}
	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->output_sequence = 0;
	else
		ctx->capture_sequence = 0;
	WRITE_ONCE(ctx->aborted, false);
	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);
	return 0;
}

static void venc_stop_streaming(struct vb2_queue *q)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(q);

	/* M2M has waited for the worker, including the DMA drain and reset. */
	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);
	venc_return_buffers(q, VB2_BUF_STATE_ERROR);
	venc_free_areas(ctx->venc);
}

static const struct vb2_ops venc_queue_ops = {
	.queue_setup = venc_queue_setup,
	.buf_prepare = venc_buf_prepare,
	.buf_queue = venc_buf_queue,
	.start_streaming = venc_start_streaming,
	.stop_streaming = venc_stop_streaming,
};

static int venc_init_queue(struct venc_ctx *ctx, struct vb2_queue *q, unsigned int type)
{
	q->type = type;
	q->io_modes = VB2_MMAP;
	q->drv_priv = ctx;
	q->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	q->ops = &venc_queue_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->lock = &ctx->venc->lock;
	q->dev = ctx->venc->dev;
	return vb2_queue_init(q);
}

static int venc_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct venc_ctx *ctx = priv;
	int ret = venc_init_queue(ctx, src, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

	return ret ?: venc_init_queue(ctx, dst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
}

static void venc_work(struct work_struct *work)
{
	struct venc_ctx *ctx = container_of(work, struct venc_ctx, work);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(m2m);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(m2m);
	const u8 *input = vb2_plane_vaddr(&src->vb2_buf, 0);
	u8 *output = vb2_plane_vaddr(&dst->vb2_buf, 0);
	int ret = -EFAULT;

	if (input && output)
		ret = venc_run(ctx, input, output, vb2_plane_size(&dst->vb2_buf, 0));
	src->sequence = ctx->output_sequence++;
	dst->sequence = ctx->capture_sequence++;
	v4l2_m2m_buf_copy_metadata(src, dst);
	dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME);
	vb2_set_plane_payload(&dst->vb2_buf, 0, ret > 0 ? ret : 0);
	if (ret > 0)
		dst->flags |= V4L2_BUF_FLAG_KEYFRAME;
	if (v4l2_m2m_is_last_draining_src_buf(m2m, src)) {
		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_mark_stopped(m2m);
	}
	v4l2_m2m_buf_done_and_job_finish(ctx->venc->m2m, m2m,
					ret > 0 ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
}

static void venc_device_run(void *priv)
{
	struct venc_ctx *ctx = priv;

	queue_work(system_dfl_long_wq, &ctx->work);
}

static void venc_job_abort(void *priv)
{
	struct venc_ctx *ctx = priv;

	WRITE_ONCE(ctx->aborted, true);
	wake_up(&ctx->venc->wait);
}

static const struct v4l2_m2m_ops venc_m2m_ops = {
	.device_run = venc_device_run,
	.job_abort = venc_job_abort,
};

static int venc_open(struct file *file)
{
	struct venc_dev *v = video_drvdata(file);
	struct venc_ctx *ctx;
	int ret;

	mutex_lock(&v->lock);
	if (v->opened || READ_ONCE(v->quarantined) || v->video.num != 2) {
		ret = READ_ONCE(v->quarantined) ? -EIO : -EBUSY;
		goto unlock;
	}
	ctx = kzalloc_obj(*ctx);
	if (!ctx) {
		ret = -ENOMEM;
		goto unlock;
	}
	ctx->venc = v;
	ctx->output.width = 1280;
	ctx->output.height = 720;
	venc_raw_format(&ctx->output);
	venc_coded_format(&ctx->capture, &ctx->output);
	INIT_WORK(&ctx->work, venc_work);
	v4l2_fh_init(&ctx->fh, &v->video);
	v4l2_ctrl_handler_init(&ctx->controls, 3);
	ctx->qp = v4l2_ctrl_new_std(&ctx->controls, NULL, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP,
				   0, 51, 1, 28);
	v4l2_ctrl_new_std_menu(&ctx->controls, NULL, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			       V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE,
			       GENMASK(0, 0), V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE);
	v4l2_ctrl_new_std(&ctx->controls, NULL, V4L2_CID_MPEG_VIDEO_GOP_SIZE, 1, 1, 1, 1);
	ret = ctx->controls.error;
	if (ret)
		goto free_ctx;
	ctx->fh.ctrl_handler = &ctx->controls;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(v->m2m, ctx, venc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto free_ctx;
	}
	v4l2_fh_add(&ctx->fh, file);
	v->opened = true;
	goto unlock;

free_ctx:
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->controls);
	kfree(ctx);
unlock:
	mutex_unlock(&v->lock);
	return ret;
}

static int venc_release(struct file *file)
{
	struct venc_ctx *ctx = venc_file_to_ctx(file);
	struct venc_dev *v = ctx->venc;

	mutex_lock(&v->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	flush_work(&ctx->work);
	venc_free_areas(v);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->controls);
	kfree(ctx);
	v->opened = false;
	mutex_unlock(&v->lock);
	return 0;
}

static const struct v4l2_file_operations venc_fops = {
	.owner = THIS_MODULE,
	.open = venc_open,
	.release = venc_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int venc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *mmio;
	struct venc_dev *v;
	int ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->dev = dev;
	v->core = devm_platform_get_and_ioremap_resource(pdev, 0, &mmio);
	if (IS_ERR(v->core))
		return PTR_ERR(v->core);
	if (resource_size(mmio) < VENC_WINDOW_SIZE)
		return -EINVAL;
	v->smmu = v->core + VENC_SMMU_OFFSET;
	v->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(v->clk))
		return dev_err_probe(dev, PTR_ERR(v->clk), "VENC clock\n");
	v->core_reset = devm_reset_control_get_exclusive(dev, "core");
	if (IS_ERR(v->core_reset))
		return dev_err_probe(dev, PTR_ERR(v->core_reset), "core reset\n");
	v->smmu_reset = devm_reset_control_get_exclusive(dev, "smmu");
	if (IS_ERR(v->smmu_reset))
		return dev_err_probe(dev, PTR_ERR(v->smmu_reset), "SMMU reset\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "32-bit DMA\n");
	mutex_init(&v->lock);
	init_waitqueue_head(&v->wait);
	v->irq = platform_get_irq(pdev, 0);
	if (v->irq < 0)
		return v->irq;
	ret = devm_request_irq(dev, v->irq, venc_irq, IRQF_NO_AUTOEN, VENC_NAME, v);
	if (ret)
		return dev_err_probe(dev, ret, "encoder IRQ\n");
	ret = v4l2_device_register(dev, &v->v4l2);
	if (ret)
		return ret;
	v->m2m = v4l2_m2m_init(&venc_m2m_ops);
	if (IS_ERR(v->m2m)) {
		ret = PTR_ERR(v->m2m);
		goto unregister_v4l2;
	}
	strscpy(v->video.name, VENC_NAME, sizeof(v->video.name));
	v->video.v4l2_dev = &v->v4l2;
	v->video.fops = &venc_fops;
	v->video.ioctl_ops = &venc_ioctls;
	v->video.release = video_device_release_empty;
	v->video.lock = &v->lock;
	v->video.vfl_dir = VFL_DIR_M2M;
	v->video.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(&v->video, v);
	ret = video_register_device(&v->video, VFL_TYPE_VIDEO, 2);
	if (ret)
		goto release_m2m;
	if (v->video.num != 2) {
		video_unregister_device(&v->video);
		ret = dev_err_probe(dev, -EBUSY, "/dev/video2 is already in use\n");
		goto release_m2m;
	}
	platform_set_drvdata(pdev, v);
	dev_info(dev, "registered as /dev/video2\n");
	return 0;

release_m2m:
	v4l2_m2m_release(v->m2m);
unregister_v4l2:
	v4l2_device_unregister(&v->v4l2);
	return ret;
}

static const struct of_device_id venc_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-venc" },
	{}
};

static struct platform_driver venc_driver = {
	.probe = venc_probe,
	.driver = {
		.name = VENC_NAME,
		.of_match_table = venc_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(venc_driver);
