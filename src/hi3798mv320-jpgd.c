// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-jpeg.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define JPGD_START		0x000
#define JPGD_RESUME		0x004
#define JPGD_STRIDE		0x00c
#define JPGD_PICTURE_SIZE	0x010
#define JPGD_PICTURE_TYPE	0x014
#define JPGD_STREAM_START	0x020
#define JPGD_STREAM_END		0x024
#define JPGD_DATA_START		0x028
#define JPGD_DATA_END		0x02c
#define JPGD_Y_ADDR		0x030
#define JPGD_UV_ADDR		0x034
#define JPGD_SCALE		0x040
#define JPGD_OUTPUT_TYPE	0x044
#define JPGD_OUTPUT_HEIGHT	0x0d0
#define JPGD_IRQ_STATUS		0x100
#define JPGD_IRQ_MASK		0x104
#define JPGD_SAMPLING		0x124
#define JPGD_RESTART_INTERVAL	0x128
#define JPGD_MMU_BYPASS		0x134
#define JPGD_QUANT		0x200
#define JPGD_DC			0x300
#define JPGD_AC_MIN		0x340
#define JPGD_AC_BASE		0x360
#define JPGD_AC_SYMBOL		0x400

#define JPGD_SMMU_CONTROL	0x000
#define JPGD_SMMU_RAW_S		0x014
#define JPGD_SMMU_RAW_NS	0x024
#define JPGD_SMMU_BYPASS	BIT(0)

#define JPGD_IRQ_DONE		BIT(0)
#define JPGD_IRQ_TERMINAL	(BIT(0) | BIT(1) | BIT(2) | BIT(4) | BIT(6))
#define JPGD_IRQ_ALL		GENMASK(6, 0)
#define JPGD_IRQ_LOW_DELAY	BIT(5)
#define JPGD_OUTPUT_UV		BIT(6)
#define JPGD_DMA_BYPASS_ALL	GENMASK(7, 0)
#define JPGD_PICTURE_420	3
#define JPGD_STREAM_ALIGN	128
#define JPGD_STRIDE_ALIGN	64
#define JPGD_MCU_SIZE		16
#define JPGD_TIMEOUT_MS		500
#define JPGD_MIN_SIZE		64
#define JPGD_MAX_SIZE		8192
#define JPGD_MAX_STREAM		SZ_32M
#define JPGD_AC_SYMBOLS		176

struct jpgd_dev {
	struct device *dev;
	void __iomem *core;
	void __iomem *smmu;
	struct clk *core_clk;
	struct clk *smmu_clk;
	struct reset_control *core_reset;
	struct reset_control *smmu_reset;
	int irq;
	u32 irq_status;
	wait_queue_head_t wait;
	struct mutex lock;
	struct v4l2_device v4l2;
	struct video_device video;
	struct v4l2_m2m_dev *m2m;
};

struct jpgd_ctx {
	struct v4l2_fh fh;
	struct jpgd_dev *jpgd;
	struct v4l2_pix_format_mplane output;
	struct v4l2_pix_format_mplane capture;
	struct work_struct work;
	bool aborted;
	u32 output_sequence;
	u32 capture_sequence;
};

struct jpgd_tables {
	const u8 *quant[3];
	u16 dc[2][12];
	u8 ac_min[2][16];
	u8 ac_base[2][16];
	u8 ac_symbol[2][JPGD_AC_SYMBOLS];
};

static const u32 jpgd_capture_formats[] = {
	V4L2_PIX_FMT_NV12,
	V4L2_PIX_FMT_NV21,
};

static struct jpgd_ctx *jpgd_file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct jpgd_ctx, fh);
}

static bool jpgd_is_output(unsigned int type)
{
	return type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

static struct v4l2_pix_format_mplane *jpgd_format(struct jpgd_ctx *ctx,
						 unsigned int type)
{
	return jpgd_is_output(type) ? &ctx->output : &ctx->capture;
}

static void jpgd_try_format(struct v4l2_pix_format_mplane *pix, bool output)
{
	u32 size = pix->plane_fmt[0].sizeimage;
	u32 stride;

	pix->width = clamp(pix->width, JPGD_MIN_SIZE, JPGD_MAX_SIZE);
	pix->height = clamp(pix->height, JPGD_MIN_SIZE, JPGD_MAX_SIZE);
	memset(pix->plane_fmt, 0, sizeof(pix->plane_fmt));
	memset(pix->reserved, 0, sizeof(pix->reserved));
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = 1;
	pix->flags = 0;
	pix->colorspace = V4L2_COLORSPACE_JPEG;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_SRGB;

	if (output) {
		pix->pixelformat = V4L2_PIX_FMT_JPEG;
		if (!size)
			size = pix->width * pix->height * 2 + JPGD_STREAM_ALIGN;
		size = clamp(size, (u32)PAGE_SIZE, (u32)JPGD_MAX_STREAM);
		pix->plane_fmt[0].sizeimage = ALIGN(size, JPGD_STREAM_ALIGN);
		return;
	}

	if (pix->pixelformat != V4L2_PIX_FMT_NV21)
		pix->pixelformat = V4L2_PIX_FMT_NV12;
	pix->width = ALIGN(pix->width, 2);
	pix->height = ALIGN(pix->height, JPGD_MCU_SIZE);
	stride = ALIGN(pix->width, JPGD_STRIDE_ALIGN);
	pix->plane_fmt[0].bytesperline = stride;
	pix->plane_fmt[0].sizeimage = stride * pix->height * 3 / 2;
}

static int jpgd_querycap(struct file *file, void *fh,
			 struct v4l2_capability *cap)
{
	struct jpgd_dev *jpgd = video_drvdata(file);

	strscpy(cap->driver, "hi3798-jpgd", sizeof(cap->driver));
	strscpy(cap->card, "Hi3798MV320 JPEG decoder", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(jpgd->dev));
	return 0;
}

static int jpgd_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		if (f->index)
			return -EINVAL;
		f->pixelformat = V4L2_PIX_FMT_JPEG;
	} else {
		if (f->index >= ARRAY_SIZE(jpgd_capture_formats))
			return -EINVAL;
		f->pixelformat = jpgd_capture_formats[f->index];
	}
	return 0;
}

static int jpgd_enum_framesizes(struct file *file, void *fh,
			       struct v4l2_frmsizeenum *f)
{
	if (f->index || f->pixel_format != V4L2_PIX_FMT_JPEG)
		return -EINVAL;

	f->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	f->stepwise.min_width = JPGD_MIN_SIZE;
	f->stepwise.min_height = JPGD_MIN_SIZE;
	f->stepwise.max_width = JPGD_MAX_SIZE;
	f->stepwise.max_height = JPGD_MAX_SIZE;
	f->stepwise.step_width = 1;
	f->stepwise.step_height = 1;
	return 0;
}

static int jpgd_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	jpgd_try_format(&f->fmt.pix_mp, jpgd_is_output(f->type));
	return 0;
}

static int jpgd_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	f->fmt.pix_mp = *jpgd_format(jpgd_file_to_ctx(file), f->type);
	return 0;
}

static int jpgd_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct jpgd_ctx *ctx = jpgd_file_to_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)))
		return -EBUSY;

	jpgd_try_fmt(file, fh, f);
	*jpgd_format(ctx, f->type) = f->fmt.pix_mp;
	return 0;
}

static int jpgd_g_selection(struct file *file, void *fh,
			    struct v4l2_selection *s)
{
	struct jpgd_ctx *ctx = jpgd_file_to_ctx(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	switch (s->target) {
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		s->r.width = min(ctx->output.width, ctx->capture.width);
		s->r.height = min(ctx->output.height, ctx->capture.height);
		break;
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		s->r.width = ctx->capture.width;
		s->r.height = ctx->capture.height;
		break;
	default:
		return -EINVAL;
	}
	s->r.left = 0;
	s->r.top = 0;
	return 0;
}

static const struct v4l2_ioctl_ops jpgd_ioctls = {
	.vidioc_querycap = jpgd_querycap,
	.vidioc_enum_fmt_vid_out = jpgd_enum_fmt,
	.vidioc_enum_fmt_vid_cap = jpgd_enum_fmt,
	.vidioc_enum_framesizes = jpgd_enum_framesizes,
	.vidioc_try_fmt_vid_out_mplane = jpgd_try_fmt,
	.vidioc_try_fmt_vid_cap_mplane = jpgd_try_fmt,
	.vidioc_g_fmt_vid_out_mplane = jpgd_g_fmt,
	.vidioc_g_fmt_vid_cap_mplane = jpgd_g_fmt,
	.vidioc_s_fmt_vid_out_mplane = jpgd_s_fmt,
	.vidioc_s_fmt_vid_cap_mplane = jpgd_s_fmt,
	.vidioc_g_selection = jpgd_g_selection,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};

static int jpgd_queue_setup(struct vb2_queue *q, unsigned int *buffers,
			    unsigned int *planes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct jpgd_ctx *ctx = vb2_get_drv_priv(q);
	u32 size = jpgd_format(ctx, q->type)->plane_fmt[0].sizeimage;

	if (*planes)
		return *planes == 1 && sizes[0] >= size ? 0 : -EINVAL;

	*planes = 1;
	sizes[0] = size;
	return 0;
}

static int jpgd_buf_prepare(struct vb2_buffer *vb)
{
	struct jpgd_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	u32 size = jpgd_format(ctx, vb->type)->plane_fmt[0].sizeimage;
	size_t used = vb2_get_plane_payload(vb, 0);

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;

	if (!jpgd_is_output(vb->type)) {
		vb2_set_plane_payload(vb, 0, 0);
		return 0;
	}

	if (vb->planes[0].data_offset || used < 4 ||
	    used > size - JPGD_STREAM_ALIGN)
		return -EINVAL;

	to_vb2_v4l2_buffer(vb)->field = V4L2_FIELD_NONE;
	return 0;
}

static void jpgd_buf_queue(struct vb2_buffer *vb)
{
	struct jpgd_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int jpgd_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct jpgd_ctx *ctx = vb2_get_drv_priv(q);

	if (jpgd_is_output(q->type))
		ctx->output_sequence = 0;
	else
		ctx->capture_sequence = 0;
	return 0;
}

static void jpgd_stop_streaming(struct vb2_queue *q)
{
	struct jpgd_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *buf;

	while ((buf = jpgd_is_output(q->type) ?
		      v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
		      v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);

	/* The mem2mem core waits for the aborted job before calling this. */
	WRITE_ONCE(ctx->aborted, false);
}

static const struct vb2_ops jpgd_queue_ops = {
	.queue_setup = jpgd_queue_setup,
	.buf_prepare = jpgd_buf_prepare,
	.buf_queue = jpgd_buf_queue,
	.start_streaming = jpgd_start_streaming,
	.stop_streaming = jpgd_stop_streaming,
};

static int jpgd_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct jpgd_ctx *ctx = priv;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src->io_modes = VB2_MMAP;
	src->drv_priv = ctx;
	src->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src->ops = &jpgd_queue_ops;
	src->mem_ops = &vb2_dma_contig_memops;
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &ctx->jpgd->lock;
	src->dev = ctx->jpgd->dev;
	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->drv_priv = ctx;
	dst->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst->ops = &jpgd_queue_ops;
	dst->mem_ops = &vb2_dma_contig_memops;
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &ctx->jpgd->lock;
	dst->dev = ctx->jpgd->dev;
	return vb2_queue_init(dst);
}

/* The JPEG helper locates the segments. Check table lengths within each marker. */
static int jpgd_read_tables(const struct v4l2_jpeg_header *header,
			    struct v4l2_jpeg_reference *quant,
			    struct v4l2_jpeg_reference *huff)
{
	unsigned int i;

	if (header->num_dqt > ARRAY_SIZE(header->dqt) ||
	    header->num_dht > ARRAY_SIZE(header->dht))
		return -EINVAL;

	for (i = 0; i < header->num_dqt; i++) {
		const struct v4l2_jpeg_reference *seg = &header->dqt[i];
		size_t pos;

		if (seg->length < 67 || (seg->length - 2) % 65)
			return -EINVAL;
		for (pos = 2; pos < seg->length; pos += 65) {
			u8 id = seg->start[pos];
			const u8 *values = seg->start + pos + 1;

			if (id >= V4L2_JPEG_MAX_TABLES || memchr(values, 0, 64))
				return -EINVAL;
			quant[id].start = seg->start + pos + 1;
		}
	}

	for (i = 0; i < header->num_dht; i++) {
		const struct v4l2_jpeg_reference *seg = &header->dht[i];
		size_t pos = 2;

		if (seg->length < 20)
			return -EINVAL;
		while (pos < seg->length) {
			unsigned int j, count = 0;
			u8 class_id, id;
			u8 *bits;

			if (seg->length - pos < 17)
				return -EINVAL;
			class_id = seg->start[pos++];
			if (class_id & ~0x11)
				return -EINVAL;
			id = (class_id >> 3) | (class_id & 1);
			bits = seg->start + pos;
			for (j = 0; j < 16; j++)
				count += bits[j];
			if (!count || count > seg->length - pos - 16 ||
			    count > (id < 2 ? 12 : 162))
				return -EINVAL;
			huff[id].start = bits;
			huff[id].length = count;
			pos += 16 + count;
		}
	}
	return 0;
}

static int jpgd_pack_huffman(struct jpgd_tables *tables,
			     const struct v4l2_jpeg_reference *table,
			     unsigned int bank, bool ac)
{
	const u8 *bits = table->start;
	const u8 *symbols;
	unsigned int len, idx = 0, code = 0;

	if (!bits)
		return -EINVAL;
	symbols = bits + 16;
	for (len = 1; len <= 16; len++) {
		unsigned int count = bits[len - 1];
		unsigned int j;

		/* Canonical JPEG codes reserve the all-ones code at every length. */
		if (code + count >= BIT(len))
			return -EINVAL;
		if (ac) {
			tables->ac_min[bank][len - 1] = idx < table->length ? code : 0xff;
			if (count)
				tables->ac_base[bank][len - 1] = idx - code;
		}
		for (j = 0; j < count; j++, idx++) {
			u8 symbol = symbols[idx];

			if (ac) {
				if ((symbol & 0xf) > 10 ||
				    (!(symbol & 0xf) && symbol != 0 && symbol != 0xf0))
					return -EINVAL;
				tables->ac_symbol[bank][idx] = symbol;
			} else {
				if (symbol >= 12 || len > 15 || tables->dc[bank][symbol])
					return -EINVAL;
				tables->dc[bank][symbol] = (len << 8) | ((code + j) & 0xff);
			}
		}
		code = (code + count) << 1;
	}
	return 0;
}

static int jpgd_parse(struct jpgd_ctx *ctx, u8 *data, size_t length,
		      struct v4l2_jpeg_header *header, struct jpgd_tables *tables)
{
	struct v4l2_jpeg_reference quant[V4L2_JPEG_MAX_TABLES] = {};
	struct v4l2_jpeg_reference huff[V4L2_JPEG_MAX_TABLES] = {};
	struct v4l2_jpeg_scan_header scan = {};
	const struct v4l2_jpeg_frame_header *frame = &header->frame;
	unsigned int i;
	int ret;

	header->scan = &scan;
	ret = v4l2_jpeg_parse_header(data, length, header);
	header->scan = NULL;
	if (ret)
		return ret;
	if (!header->sof.start || header->sof.start[-1] != 0xc0 ||
	    frame->precision != 8 || frame->num_components != 3 ||
	    frame->subsampling != V4L2_JPEG_CHROMA_SUBSAMPLING_420 ||
	    header->app14_tf == V4L2_JPEG_APP14_TF_CMYK_RGB ||
	    scan.num_components != 3 || header->sos.length != 12 ||
	    header->sos.start[9] != 0 || header->sos.start[10] != 63 ||
	    header->sos.start[11] != 0 || header->ecs_offset >= length - 2 ||
	    data[length - 2] != 0xff || data[length - 1] != 0xd9)
		return -EINVAL;
	if (frame->width != ctx->output.width || frame->height != ctx->output.height ||
	    ALIGN(frame->width, 2) != ctx->capture.width ||
	    ALIGN(frame->height, JPGD_MCU_SIZE) != ctx->capture.height)
		return -EINVAL;

	ret = jpgd_read_tables(header, quant, huff);
	if (ret)
		return ret;

	for (i = 0; i < 3; i++) {
		const struct v4l2_jpeg_frame_component_spec *comp = &frame->component[i];
		const struct v4l2_jpeg_scan_component_spec *sel = &scan.component[i];
		unsigned int q = comp->quantization_table_selector;
		unsigned int dc = sel->dc_entropy_coding_table_selector;
		unsigned int ac = sel->ac_entropy_coding_table_selector;
		unsigned int sampling = i ? 1 : 2;

		if (comp->horizontal_sampling_factor != sampling ||
		    comp->vertical_sampling_factor != sampling ||
		    sel->component_selector != comp->component_identifier ||
		    q >= ARRAY_SIZE(quant) || !quant[q].start || dc > 1 || ac > 1)
			return -EINVAL;
		tables->quant[i] = quant[q].start;
		if (i == 2) {
			/* Cb and Cr share the hardware chroma Huffman bank. */
			if (dc != scan.component[1].dc_entropy_coding_table_selector ||
			    ac != scan.component[1].ac_entropy_coding_table_selector)
				return -EINVAL;
			break;
		}
		ret = jpgd_pack_huffman(tables, &huff[dc], i, false);
		if (ret)
			return ret;
		ret = jpgd_pack_huffman(tables, &huff[2 + ac], i, true);
		if (ret)
			return ret;
	}
	if (frame->component[0].component_identifier == frame->component[1].component_identifier ||
	    frame->component[0].component_identifier == frame->component[2].component_identifier ||
	    frame->component[1].component_identifier == frame->component[2].component_identifier)
		return -EINVAL;

	return 0;
}

static void jpgd_write_tables(struct jpgd_dev *jpgd, const struct jpgd_tables *tables)
{
	unsigned int i;

	for (i = 0; i < V4L2_JPEG_PIXELS_IN_BLOCK; i++) {
		u8 index = v4l2_jpeg_zigzag_scan_index[i];
		u32 val = tables->quant[0][i] | (tables->quant[1][i] << 8) |
			  (tables->quant[2][i] << 16);

		writel_relaxed(val, jpgd->core + JPGD_QUANT + 4 * index);
	}
	for (i = 0; i < 12; i++)
		writel_relaxed(tables->dc[0][i] | ((u32)tables->dc[1][i] << 12),
			       jpgd->core + JPGD_DC + 4 * i);
	for (i = 0; i < 8; i++) {
		u32 min = tables->ac_min[0][2 * i] | (tables->ac_min[0][2 * i + 1] << 8) |
			  (tables->ac_min[1][2 * i] << 16) | ((u32)tables->ac_min[1][2 * i + 1] << 24);
		u32 base = tables->ac_base[0][2 * i] | (tables->ac_base[0][2 * i + 1] << 8) |
			   (tables->ac_base[1][2 * i] << 16) | ((u32)tables->ac_base[1][2 * i + 1] << 24);

		writel_relaxed(min, jpgd->core + JPGD_AC_MIN + 4 * i);
		writel_relaxed(base, jpgd->core + JPGD_AC_BASE + 4 * i);
	}
	for (i = 0; i < JPGD_AC_SYMBOLS; i++)
		writel_relaxed(tables->ac_symbol[0][i] | (tables->ac_symbol[1][i] << 8),
			       jpgd->core + JPGD_AC_SYMBOL + 4 * i);
}

static int jpgd_power_on(struct jpgd_dev *jpgd)
{
	int ret;

	ret = clk_prepare_enable(jpgd->smmu_clk);
	if (ret)
		return ret;
	ret = clk_prepare_enable(jpgd->core_clk);
	if (ret)
		goto disable_smmu;
	udelay(5);
	ret = reset_control_deassert(jpgd->smmu_reset);
	if (ret)
		goto disable_core;
	ret = reset_control_deassert(jpgd->core_reset);
	if (ret)
		goto reset_smmu;
	udelay(5);

	writel(readl(jpgd->smmu + JPGD_SMMU_CONTROL) | JPGD_SMMU_BYPASS,
	       jpgd->smmu + JPGD_SMMU_CONTROL);
	if (readl(jpgd->smmu + JPGD_SMMU_CONTROL) & JPGD_SMMU_BYPASS)
		return 0;

	ret = -EIO;
	reset_control_assert(jpgd->core_reset);
reset_smmu:
	reset_control_assert(jpgd->smmu_reset);
disable_core:
	clk_disable_unprepare(jpgd->core_clk);
disable_smmu:
	clk_disable_unprepare(jpgd->smmu_clk);
	return ret;
}

static void jpgd_power_off(struct jpgd_dev *jpgd)
{
	reset_control_assert(jpgd->core_reset);
	udelay(5);
	reset_control_assert(jpgd->smmu_reset);
	udelay(5);
	clk_disable_unprepare(jpgd->core_clk);
	clk_disable_unprepare(jpgd->smmu_clk);
}

static irqreturn_t jpgd_irq(int irq, void *priv)
{
	struct jpgd_dev *jpgd = priv;
	u32 status = readl(jpgd->core + JPGD_IRQ_STATUS) & JPGD_IRQ_ALL;

	if (!status)
		return IRQ_NONE;

	writel(status, jpgd->core + JPGD_IRQ_STATUS);
	readl(jpgd->core + JPGD_IRQ_STATUS);
	if (status & JPGD_IRQ_TERMINAL) {
		WRITE_ONCE(jpgd->irq_status, READ_ONCE(jpgd->irq_status) | status);
		wake_up(&jpgd->wait);
	}
	return IRQ_HANDLED;
}

static int jpgd_decode(struct jpgd_ctx *ctx, struct vb2_buffer *src,
		       struct vb2_buffer *dst)
{
	struct jpgd_dev *jpgd = ctx->jpgd;
	struct v4l2_jpeg_header header = {};
	struct jpgd_tables tables = {};
	const struct v4l2_pix_format_mplane *cap = &ctx->capture;
	u32 stride = cap->plane_fmt[0].bytesperline;
	u32 output_type = cap->pixelformat == V4L2_PIX_FMT_NV12 ? JPGD_OUTPUT_UV : 0;
	dma_addr_t src_dma = vb2_dma_contig_plane_dma_addr(src, 0);
	dma_addr_t dst_dma = vb2_dma_contig_plane_dma_addr(dst, 0);
	size_t length = vb2_get_plane_payload(src, 0);
	u64 stream_end = ALIGN((u64)src_dma + length, JPGD_STREAM_ALIGN);
	u8 *data = vb2_plane_vaddr(src, 0);
	u32 status, smmu_s, smmu_ns;
	long completed;
	int ret;

	if (!data || !IS_ALIGNED(src_dma, JPGD_STREAM_ALIGN) ||
	    !IS_ALIGNED(dst_dma, JPGD_STRIDE_ALIGN) || stream_end > U32_MAX ||
	    (u64)dst_dma + cap->plane_fmt[0].sizeimage - 1 > U32_MAX)
		return -EINVAL;

	ret = jpgd_parse(ctx, data, length, &header, &tables);
	if (ret || READ_ONCE(ctx->aborted))
		return ret ?: -ECANCELED;

	ret = jpgd_power_on(jpgd);
	if (ret) {
		dev_err_ratelimited(jpgd->dev, "power or SMMU bypass setup failed: %d\n", ret);
		return ret;
	}

	jpgd_write_tables(jpgd, &tables);
	writel_relaxed(DIV_ROUND_UP(header.frame.width, JPGD_MCU_SIZE) |
		       (DIV_ROUND_UP(header.frame.height, JPGD_MCU_SIZE) << 16),
		       jpgd->core + JPGD_PICTURE_SIZE);
	writel_relaxed(JPGD_PICTURE_420, jpgd->core + JPGD_PICTURE_TYPE);
	/* Full-size decode, internal clock enable and three outstanding requests. */
	writel_relaxed(0x3c, jpgd->core + JPGD_SCALE);
	writel_relaxed(output_type, jpgd->core + JPGD_OUTPUT_TYPE);
	writel_relaxed(cap->height, jpgd->core + JPGD_OUTPUT_HEIGHT);
	writel_relaxed(0x221111, jpgd->core + JPGD_SAMPLING);
	writel_relaxed(header.restart_interval, jpgd->core + JPGD_RESTART_INTERVAL);
	writel_relaxed(stride | (stride << 16), jpgd->core + JPGD_STRIDE);
	writel_relaxed(src_dma + header.ecs_offset, jpgd->core + JPGD_DATA_START);
	writel_relaxed(src_dma + length, jpgd->core + JPGD_DATA_END);
	writel_relaxed(round_down(src_dma + header.ecs_offset, JPGD_STREAM_ALIGN),
		       jpgd->core + JPGD_STREAM_START);
	writel_relaxed(stream_end, jpgd->core + JPGD_STREAM_END);
	writel_relaxed(dst_dma, jpgd->core + JPGD_Y_ADDR);
	writel_relaxed(dst_dma + stride * cap->height, jpgd->core + JPGD_UV_ADDR);
	/* Physical DMA on the input, Y and UV channels. */
	writel_relaxed(JPGD_DMA_BYPASS_ALL, jpgd->core + JPGD_MMU_BYPASS);
	writel_relaxed(JPGD_IRQ_ALL, jpgd->core + JPGD_IRQ_STATUS);
	writel_relaxed(JPGD_IRQ_LOW_DELAY, jpgd->core + JPGD_IRQ_MASK);
	writel(BIT(1), jpgd->core + JPGD_RESUME);
	WRITE_ONCE(jpgd->irq_status, 0);
	enable_irq(jpgd->irq);

	if (!READ_ONCE(ctx->aborted))
		writel(BIT(0), jpgd->core + JPGD_START);
	completed = wait_event_timeout(jpgd->wait, READ_ONCE(ctx->aborted) ||
				       READ_ONCE(jpgd->irq_status),
				       msecs_to_jiffies(JPGD_TIMEOUT_MS));

	disable_irq(jpgd->irq);
	status = READ_ONCE(jpgd->irq_status);
	smmu_s = readl(jpgd->smmu + JPGD_SMMU_RAW_S);
	smmu_ns = readl(jpgd->smmu + JPGD_SMMU_RAW_NS);
	writel(JPGD_IRQ_ALL, jpgd->core + JPGD_IRQ_MASK);
	writel(JPGD_IRQ_ALL, jpgd->core + JPGD_IRQ_STATUS);
	/* Reset the DMA master before releasing either buffer, including on abort. */
	jpgd_power_off(jpgd);

	if (smmu_s || smmu_ns) {
		dev_err_ratelimited(jpgd->dev, "SMMU fault: secure=%#x non-secure=%#x\n",
				    smmu_s, smmu_ns);
		return -EIO;
	}
	if (READ_ONCE(ctx->aborted))
		return -ECANCELED;
	if (!completed) {
		dev_err_ratelimited(jpgd->dev, "decode timed out\n");
		return -ETIMEDOUT;
	}
	if ((status & JPGD_IRQ_TERMINAL) != JPGD_IRQ_DONE) {
		dev_dbg(jpgd->dev, "decode failed with status %#x\n", status);
		return -EIO;
	}
	return 0;
}

static void jpgd_work(struct work_struct *work)
{
	struct jpgd_ctx *ctx = container_of(work, struct jpgd_ctx, work);
	struct jpgd_dev *jpgd = ctx->jpgd;
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(m2m);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(m2m);
	int ret;

	ret = jpgd_decode(ctx, &src->vb2_buf, &dst->vb2_buf);
	if (!ret)
		vb2_set_plane_payload(&dst->vb2_buf, 0, ctx->capture.plane_fmt[0].sizeimage);
	src->sequence = ctx->output_sequence++;
	dst->sequence = ctx->capture_sequence++;
	v4l2_m2m_buf_copy_metadata(src, dst);
	v4l2_m2m_buf_done_and_job_finish(jpgd->m2m, m2m,
					ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
}

static void jpgd_device_run(void *priv)
{
	struct jpgd_ctx *ctx = priv;

	queue_work(system_dfl_long_wq, &ctx->work);
}

static void jpgd_job_abort(void *priv)
{
	struct jpgd_ctx *ctx = priv;

	WRITE_ONCE(ctx->aborted, true);
	wake_up(&ctx->jpgd->wait);
}

static const struct v4l2_m2m_ops jpgd_m2m_ops = {
	.device_run = jpgd_device_run,
	.job_abort = jpgd_job_abort,
};

static int jpgd_open(struct file *file)
{
	struct jpgd_dev *jpgd = video_drvdata(file);
	struct jpgd_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->jpgd = jpgd;
	ctx->output.width = 1280;
	ctx->output.height = 720;
	ctx->capture = ctx->output;
	jpgd_try_format(&ctx->output, true);
	jpgd_try_format(&ctx->capture, false);
	INIT_WORK(&ctx->work, jpgd_work);
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(jpgd->m2m, ctx, jpgd_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);
		return ret;
	}
	v4l2_fh_add(&ctx->fh, file);
	return 0;
}

static int jpgd_release(struct file *file)
{
	struct jpgd_ctx *ctx = jpgd_file_to_ctx(file);

	mutex_lock(&ctx->jpgd->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	flush_work(&ctx->work);
	mutex_unlock(&ctx->jpgd->lock);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations jpgd_fops = {
	.owner = THIS_MODULE,
	.open = jpgd_open,
	.release = jpgd_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int jpgd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct jpgd_dev *jpgd;
	int ret;

	jpgd = devm_kzalloc(dev, sizeof(*jpgd), GFP_KERNEL);
	if (!jpgd)
		return -ENOMEM;
	jpgd->dev = dev;
	jpgd->core = devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(jpgd->core))
		return PTR_ERR(jpgd->core);
	jpgd->smmu = devm_platform_ioremap_resource_byname(pdev, "smmu");
	if (IS_ERR(jpgd->smmu))
		return PTR_ERR(jpgd->smmu);
	jpgd->core_clk = devm_clk_get(dev, "core");
	if (IS_ERR(jpgd->core_clk))
		return dev_err_probe(dev, PTR_ERR(jpgd->core_clk), "core clock\n");
	jpgd->smmu_clk = devm_clk_get(dev, "smmu");
	if (IS_ERR(jpgd->smmu_clk))
		return dev_err_probe(dev, PTR_ERR(jpgd->smmu_clk), "SMMU clock\n");
	jpgd->core_reset = devm_reset_control_get_exclusive(dev, "core");
	if (IS_ERR(jpgd->core_reset))
		return dev_err_probe(dev, PTR_ERR(jpgd->core_reset), "core reset\n");
	jpgd->smmu_reset = devm_reset_control_get_exclusive(dev, "smmu");
	if (IS_ERR(jpgd->smmu_reset))
		return dev_err_probe(dev, PTR_ERR(jpgd->smmu_reset), "SMMU reset\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "32-bit DMA\n");

	mutex_init(&jpgd->lock);
	init_waitqueue_head(&jpgd->wait);
	jpgd->irq = platform_get_irq(pdev, 0);
	if (jpgd->irq < 0)
		return jpgd->irq;
	ret = devm_request_irq(dev, jpgd->irq, jpgd_irq, IRQF_NO_AUTOEN,
			       "hi3798mv320-jpgd", jpgd);
	if (ret)
		return dev_err_probe(dev, ret, "decode IRQ\n");
	ret = reset_control_assert(jpgd->core_reset);
	if (ret)
		return ret;
	ret = reset_control_assert(jpgd->smmu_reset);
	if (ret)
		return ret;

	ret = v4l2_device_register(dev, &jpgd->v4l2);
	if (ret)
		return ret;
	jpgd->m2m = v4l2_m2m_init(&jpgd_m2m_ops);
	if (IS_ERR(jpgd->m2m)) {
		ret = PTR_ERR(jpgd->m2m);
		goto unregister_v4l2;
	}

	strscpy(jpgd->video.name, "hi3798mv320-jpgd", sizeof(jpgd->video.name));
	jpgd->video.v4l2_dev = &jpgd->v4l2;
	jpgd->video.fops = &jpgd_fops;
	jpgd->video.ioctl_ops = &jpgd_ioctls;
	jpgd->video.release = video_device_release_empty;
	jpgd->video.lock = &jpgd->lock;
	jpgd->video.vfl_dir = VFL_DIR_M2M;
	jpgd->video.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(&jpgd->video, jpgd);
	ret = video_register_device(&jpgd->video, VFL_TYPE_VIDEO, 1);
	if (ret)
		goto release_m2m;

	platform_set_drvdata(pdev, jpgd);
	dev_info(dev, "registered as /dev/%s\n", video_device_node_name(&jpgd->video));
	return 0;

release_m2m:
	v4l2_m2m_release(jpgd->m2m);
unregister_v4l2:
	v4l2_device_unregister(&jpgd->v4l2);
	return ret;
}

static void jpgd_remove(struct platform_device *pdev)
{
	struct jpgd_dev *jpgd = platform_get_drvdata(pdev);

	video_unregister_device(&jpgd->video);
	v4l2_m2m_release(jpgd->m2m);
	v4l2_device_unregister(&jpgd->v4l2);
}

static const struct of_device_id jpgd_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-jpgd" },
	{ }
};

static struct platform_driver jpgd_driver = {
	.probe = jpgd_probe,
	.remove = jpgd_remove,
	.driver = {
		.name = "hi3798mv320-jpgd",
		.of_match_table = jpgd_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(jpgd_driver);
