/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_drm.h>

#include "hi3798mv320-tiled.h"

enum { FB, CRTC, SX, SY, SW, SH, DX, DY, DW, DH, PLANE_PROPS };
static const char *const plane_props[] = {
	"FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
	"CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H",
};

struct overlay {
	struct drm_mode_create_dumb dumb;
	uint32_t fb;
	uint8_t *pixels;
};

struct display {
	int fd;
	uint32_t connector, crtc, v0, g0, mode_blob;
	drmModeModeInfo mode;
	uint32_t conn_crtc, crtc_mode, crtc_active, video[PLANE_PROPS], osd[PLANE_PROPS];
	uint32_t color, range, blend;
	uint64_t bt601, bt709, bt2020, limited, full, coverage, active_encoding, active_range;
	struct overlay overlays[2];
	int front, pending_overlay, progress;
	AVFrame *current, *pending;
	uint32_t current_fb, pending_fb, sequence, missed;
	uint64_t first_us, last_us, segment_us;
	unsigned int shown, segment_shown, osd_flips;
	int64_t base_pts;
	bool active;
};

enum { DECODE_AHEAD = 5 };

struct input {
	AVFormatContext *format;
	AVCodecContext *decoder;
	AVPacket *packet;
	AVStream *video;
	AVRational time_base, sar;
	enum AVColorSpace space;
	enum AVColorRange range;
	int index;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	AVFrame *queue[DECODE_AHEAD];
	unsigned int head, count;
	int status;
	int64_t start, seek_target;
	bool quit, seek_pending;
};

static volatile sig_atomic_t stopped;

static void interrupt(int sig)
{
	(void)sig;
	stopped = 1;
}

static _Noreturn void fail(const char *what)
{
	fprintf(stderr, "%s\n", what);
	exit(EXIT_FAILURE);
}

static void check_drm(int ret, const char *what)
{
	if (ret < 0) {
		perror(what);
		exit(EXIT_FAILURE);
	}
}

static void check_av(int ret, const char *what)
{
	char message[AV_ERROR_MAX_STRING_SIZE];

	if (ret < 0) {
		av_strerror(ret, message, sizeof(message));
		fprintf(stderr, "%s: %s\n", what, message);
		exit(EXIT_FAILURE);
	}
}

static uint64_t now_us(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000000 + time.tv_nsec / 1000;
}

static void sleep_until(uint64_t us)
{
	struct timespec time = { .tv_sec = us / 1000000, .tv_nsec = us % 1000000 * 1000 };

	while (!stopped && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, NULL) == EINTR)
		;
}

static uint32_t property(int fd, uint32_t object, uint32_t type, const char *name, uint64_t *value)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, object, type);
	uint32_t id = 0;

	if (!props)
		fail("cannot read KMS object properties");
	for (uint32_t i = 0; i < props->count_props && !id; i++) {
		drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);

		if (prop && !strcmp(prop->name, name)) {
			id = prop->prop_id;
			if (value)
				*value = props->prop_values[i];
		}
		drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);
	if (!id)
		fail(name);
	return id;
}

static uint64_t enum_value(int fd, uint32_t property_id, const char *name)
{
	drmModePropertyRes *prop = drmModeGetProperty(fd, property_id);

	if (prop) {
		for (int i = 0; i < prop->count_enums; i++) {
			if (!strcmp(prop->enums[i].name, name)) {
				uint64_t value = prop->enums[i].value;

				drmModeFreeProperty(prop);
				return value;
			}
		}
	}
	drmModeFreeProperty(prop);
	fail(name);
	return 0;
}

static void atomic_add(drmModeAtomicReq *req, uint32_t id, uint32_t prop, uint64_t value)
{
	check_drm(drmModeAtomicAddProperty(req, id, prop, value), "add KMS property");
}

static void set_plane(drmModeAtomicReq *req, uint32_t id, const uint32_t *props,
		      const uint64_t values[PLANE_PROPS])
{
	for (int i = 0; i < PLANE_PROPS; i++)
		atomic_add(req, id, props[i], values[i]);
}

static void create_overlay(struct display *d, struct overlay *osd)
{
	struct drm_mode_map_dumb map = {};
	uint32_t handles[4] = {}, pitches[4] = {}, offsets[4] = {};

	osd->dumb = (struct drm_mode_create_dumb){ .width = 1920, .height = 1080, .bpp = 32 };
	check_drm(drmIoctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &osd->dumb), "create G0 buffer");
	map.handle = handles[0] = osd->dumb.handle;
	pitches[0] = osd->dumb.pitch;
	check_drm(drmModeAddFB2(d->fd, 1920, 1080, DRM_FORMAT_ARGB8888, handles, pitches,
			       offsets, &osd->fb, 0), "add G0 framebuffer");
	check_drm(drmIoctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &map), "map G0 buffer");
	osd->pixels = mmap(NULL, osd->dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, map.offset);
	if (osd->pixels == MAP_FAILED)
		fail("cannot map G0 canvas");
	memset(osd->pixels, 0, osd->dumb.size);
}

static void draw_progress(struct overlay *osd, int progress)
{
	/* Coverage alpha. Outside the status panel the canvas stays transparent. */
	for (unsigned int y = 950; y < 1030; y++) {
		uint32_t *row = (void *)(osd->pixels + y * osd->dumb.pitch);

		for (unsigned int x = 160; x < 1760; x++) {
			uint32_t color = 0x90000000;

			if (x >= 192 && x < 1728 && y >= 985 && y < 998)
				color = 0xa0ffffff;
			if (x >= 192 && x < 202U + 1526U * progress / 1000 && y >= 981 && y < 1002)
				color = 0xff49d3f2;
			row[x] = color;
		}
	}
}

static void open_display(struct display *d, const char *path, bool show_osd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	drmModePlaneRes *planes;
	uint64_t clock_type;

	d->front = d->pending_overlay = d->progress = -1;
	d->fd = open(path, O_RDWR | O_CLOEXEC);
	check_drm(d->fd, path);
	check_drm(drmSetClientCap(d->fd, DRM_CLIENT_CAP_ATOMIC, 1), "enable atomic KMS");
	check_drm(drmGetCap(d->fd, DRM_CAP_TIMESTAMP_MONOTONIC, &clock_type), "get KMS event clock");
	if (!clock_type)
		fail("KMS does not provide monotonic flip timestamps");
	res = drmModeGetResources(d->fd);
	if (!res || res->count_connectors != 1 || res->count_crtcs != 1)
		fail("expected the HG680KD handoff connector and CRTC");
	d->connector = res->connectors[0];
	d->crtc = res->crtcs[0];
	drmModeFreeResources(res);
	conn = drmModeGetConnector(d->fd, d->connector);
	if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes != 1)
		fail("firmware HDMI handoff mode is unavailable");
	d->mode = conn->modes[0];
	drmModeFreeConnector(conn);
	check_drm(drmModeCreatePropertyBlob(d->fd, &d->mode, sizeof(d->mode), &d->mode_blob),
		  "create firmware mode blob");
	planes = drmModeGetPlaneResources(d->fd);
	if (!planes)
		fail("cannot enumerate KMS planes");
	for (uint32_t i = 0; i < planes->count_planes; i++) {
		uint32_t id = planes->planes[i];
		uint64_t type;

		property(d->fd, id, DRM_MODE_OBJECT_PLANE, "type", &type);
		if (type == DRM_PLANE_TYPE_PRIMARY)
			d->v0 = id;
		if (type == DRM_PLANE_TYPE_OVERLAY)
			d->g0 = id;
	}
	drmModeFreePlaneResources(planes);
	if (!d->v0 || (show_osd && !d->g0) || !d->mode.vrefresh)
		fail("missing V0, G0, or refresh rate");
	d->conn_crtc = property(d->fd, d->connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
	d->crtc_mode = property(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
	d->crtc_active = property(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
	for (int i = 0; i < PLANE_PROPS; i++)
		d->video[i] = property(d->fd, d->v0, DRM_MODE_OBJECT_PLANE, plane_props[i], NULL);
	d->color = property(d->fd, d->v0, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING", NULL);
	d->range = property(d->fd, d->v0, DRM_MODE_OBJECT_PLANE, "COLOR_RANGE", NULL);
	d->bt601 = enum_value(d->fd, d->color, "ITU-R BT.601 YCbCr");
	d->bt709 = enum_value(d->fd, d->color, "ITU-R BT.709 YCbCr");
	d->bt2020 = enum_value(d->fd, d->color, "ITU-R BT.2020 YCbCr");
	d->limited = enum_value(d->fd, d->range, "YCbCr limited range");
	d->full = enum_value(d->fd, d->range, "YCbCr full range");
	printf("KMS connector=%u CRTC=%u V0=%u mode=%ux%u@%u\n", d->connector, d->crtc,
	       d->v0, d->mode.hdisplay, d->mode.vdisplay, d->mode.vrefresh);
	if (show_osd) {
		if (d->mode.hdisplay != 3840 || d->mode.vdisplay != 2160)
			fail("G0 OSD requires the 3840x2160 firmware mode");
		for (int i = 0; i < PLANE_PROPS; i++)
			d->osd[i] = property(d->fd, d->g0, DRM_MODE_OBJECT_PLANE, plane_props[i], NULL);
		d->blend = property(d->fd, d->g0, DRM_MODE_OBJECT_PLANE, "pixel blend mode", NULL);
		d->coverage = enum_value(d->fd, d->blend, "Coverage");
		for (int i = 0; i < 2; i++)
			create_overlay(d, &d->overlays[i]);
		printf("G0 plane=%u ARGB8888 1920x1080 -> 3840x2160 blend=Coverage\n", d->g0);
	}
}

static uint32_t import_frame(struct display *d, const AVFrame *frame)
{
	const AVDRMFrameDescriptor *desc = (const void *)frame->data[0];
	const AVDRMLayerDescriptor *layer;
	uint32_t handles[4] = {}, pitches[4] = {}, offsets[4] = {}, fb, handle;
	uint64_t modifiers[4] = {};
	int ret;

	if (frame->format != AV_PIX_FMT_DRM_PRIME || !desc || desc->nb_objects != 1 ||
	    desc->nb_layers != 1 || desc->objects[0].format_modifier != MV320_TILED_MODIFIER)
		fail("decoder did not produce a native MV320 DRM PRIME frame");
	layer = &desc->layers[0];
	if (!((layer->format == DRM_FORMAT_NV21 && layer->nb_planes == 2) ||
	      (layer->format == MV320_TILED_10 && layer->nb_planes == 4)))
		fail("unexpected native MV320 DRM plane layout");
	check_drm(drmPrimeFDToHandle(d->fd, desc->objects[0].fd, &handle), "import VDEC dma-buf");
	for (int i = 0; i < layer->nb_planes; i++) {
		handles[i] = handle;
		pitches[i] = layer->planes[i].pitch;
		offsets[i] = layer->planes[i].offset;
		modifiers[i] = desc->objects[0].format_modifier;
	}
	ret = drmModeAddFB2WithModifiers(d->fd, frame->width, frame->height, layer->format, handles,
					 pitches, offsets, modifiers, &fb, DRM_MODE_FB_MODIFIERS);
	check_drm(ret, "add native V0 framebuffer");
	struct drm_gem_close close_handle = { .handle = handle };

	check_drm(drmIoctl(d->fd, DRM_IOCTL_GEM_CLOSE, &close_handle), "close imported GEM handle");
	return fb;
}

static void submit_frame(struct display *d, AVFrame *frame, const struct input *in, int progress)
{
	const AVDRMFrameDescriptor *desc = (const void *)frame->data[0];
	uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	AVRational sar = frame->sample_aspect_ratio;
	unsigned int width = d->mode.hdisplay, height = d->mode.vdisplay;
	int64_t dar_num, dar_den;
	enum AVColorSpace space = frame->colorspace;
	enum AVColorRange range = frame->color_range;
	uint64_t encoding, kms_range;

	if (!req)
		fail("cannot allocate atomic request");
	if (sar.num <= 0 || sar.den <= 0)
		sar = in->sar;
	if (sar.num <= 0 || sar.den <= 0)
		sar = (AVRational){ 1, 1 };
	dar_num = (int64_t)frame->width * sar.num;
	dar_den = (int64_t)frame->height * sar.den;
	if (dar_num * height > dar_den * width)
		height = width * dar_den / dar_num & ~1U;
	else
		width = height * dar_num / dar_den & ~1U;
	if (frame->crop_left || frame->crop_top || frame->crop_right || frame->crop_bottom)
		fail("frame has unapplied cropping");
	if (space == AVCOL_SPC_UNSPECIFIED)
		space = in->space;
	if (range == AVCOL_RANGE_UNSPECIFIED)
		range = in->range;
	switch (space) {
	case AVCOL_SPC_BT2020_NCL: encoding = d->bt2020; break;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M: encoding = d->bt601; break;
	case AVCOL_SPC_BT709: encoding = d->bt709; break;
	case AVCOL_SPC_UNSPECIFIED: encoding = frame->height > 576 ? d->bt709 : d->bt601; break;
	default: fail("unsupported YCbCr matrix for SDR KMS");
	}
	kms_range = range == AVCOL_RANGE_JPEG ? d->full : d->limited;

	d->pending_fb = import_frame(d, frame);
	uint64_t values[PLANE_PROPS] = {
		d->pending_fb, d->crtc, 0, 0, (uint64_t)frame->width << 16,
		(uint64_t)frame->height << 16, (d->mode.hdisplay - width) / 2 & ~1U,
		(d->mode.vdisplay - height) / 2 & ~1U, width, height,
	};
	set_plane(req, d->v0, d->video, values);
	atomic_add(req, d->v0, d->color, encoding);
	atomic_add(req, d->v0, d->range, kms_range);
	if (!d->active || d->active_encoding != encoding || d->active_range != kms_range) {
		printf("V0 color COLOR_ENCODING=%" PRIu64 " COLOR_RANGE=%" PRIu64 "\n",
		       encoding, kms_range);
		d->active_encoding = encoding;
		d->active_range = kms_range;
	}
	if (d->osd[FB] && (d->progress < 0 || abs(progress - d->progress) >= 10)) {
		int index = d->front == 0 ? 1 : 0;
		struct overlay *osd = &d->overlays[index];
		uint64_t osd_values[PLANE_PROPS] = {
			osd->fb, d->crtc, 0, 0, 1920U << 16, 1080U << 16, 0, 0, 3840, 2160,
		};

		draw_progress(osd, progress);
		if (d->front < 0) {
			set_plane(req, d->g0, d->osd, osd_values);
			atomic_add(req, d->g0, d->blend, d->coverage);
		} else {
			atomic_add(req, d->g0, d->osd[FB], osd->fb);
		}
		d->pending_overlay = index;
		d->progress = progress;
	}
	if (!d->active) {
		const AVDRMLayerDescriptor *layer = &desc->layers[0];

		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
		atomic_add(req, d->connector, d->conn_crtc, d->crtc);
		atomic_add(req, d->crtc, d->crtc_mode, d->mode_blob);
		atomic_add(req, d->crtc, d->crtc_active, 1);
		printf("V0 source=%dx%d destination=%ux%u fourcc=%s modifier=0x%016" PRIx64
		       " objects=%d planes=%d\n",
		       frame->width, frame->height, width, height, av_fourcc2str(layer->format),
		       desc->objects[0].format_modifier, desc->nb_objects, layer->nb_planes);
		for (int i = 0; i < layer->nb_planes; i++)
			printf("DRM plane[%d] offset=%td pitch=%td\n", i,
			       layer->planes[i].offset, layer->planes[i].pitch);
	}
	check_drm(drmModeAtomicCommit(d->fd, req, flags, d), "submit V0/G0 atomic flip");
	drmModeAtomicFree(req);
	av_frame_move_ref(d->pending, frame);
	d->active = true;
}

static void complete_flip(struct display *d)
{
	struct pollfd pfd = { .fd = d->fd, .events = POLLIN };
	struct drm_event_vblank event;
	uint64_t us;
	int ret;

	if (!d->pending_fb)
		return;
	do {
		ret = poll(&pfd, 1, 2000);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0 || read(d->fd, &event, sizeof(event)) != sizeof(event) ||
	    event.base.type != DRM_EVENT_FLIP_COMPLETE || event.base.length != sizeof(event) ||
	    event.crtc_id != d->crtc || event.user_data != (uintptr_t)d)
		fail("missing or invalid KMS flip completion");
	us = (uint64_t)event.tv_sec * 1000000 + event.tv_usec;
	if (!d->shown)
		d->first_us = us;
	if (!d->segment_shown)
		d->segment_us = us;
	else if ((int32_t)(event.sequence - d->sequence) <= 0)
		fail("KMS flip sequence did not advance");
	else
		d->missed += event.sequence - d->sequence - 1;
	d->sequence = event.sequence;
	d->last_us = us;
	d->shown++;
	d->segment_shown++;
	if (d->pending_overlay >= 0) {
		d->front = d->pending_overlay;
		d->pending_overlay = -1;
		d->osd_flips++;
	}
	/* The previous frame can return to the decoder pool only after this latch. */
	if (d->current_fb)
		check_drm(drmModeRmFB(d->fd, d->current_fb), "remove retired V0 framebuffer");
	av_frame_unref(d->current);
	av_frame_move_ref(d->current, d->pending);
	d->current_fb = d->pending_fb;
	d->pending_fb = 0;
}

static void close_display(struct display *d)
{
	if (d->active) {
		drmModeAtomicReq *req = drmModeAtomicAlloc();

		if (!req)
			fail("cannot allocate KMS disable request");
		atomic_add(req, d->v0, d->video[FB], 0);
		atomic_add(req, d->v0, d->video[CRTC], 0);
		if (d->front >= 0) {
			atomic_add(req, d->g0, d->osd[FB], 0);
			atomic_add(req, d->g0, d->osd[CRTC], 0);
		}
		atomic_add(req, d->connector, d->conn_crtc, 0);
		atomic_add(req, d->crtc, d->crtc_active, 0);
		atomic_add(req, d->crtc, d->crtc_mode, 0);
		check_drm(drmModeAtomicCommit(d->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL),
			  "disable V0/G0");
		drmModeAtomicFree(req);
		check_drm(drmModeRmFB(d->fd, d->current_fb), "remove last V0 framebuffer");
	}
	for (int i = 0; i < 2 && d->osd[FB]; i++) {
		struct overlay *osd = &d->overlays[i];
		struct drm_mode_destroy_dumb destroy = { .handle = osd->dumb.handle };

		munmap(osd->pixels, osd->dumb.size);
		check_drm(drmModeRmFB(d->fd, osd->fb), "remove G0 framebuffer");
		check_drm(drmIoctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy), "destroy G0 buffer");
	}
	drmModeDestroyPropertyBlob(d->fd, d->mode_blob);
	close(d->fd);
	av_frame_free(&d->current);
	av_frame_free(&d->pending);
}

static enum AVPixelFormat choose_format(AVCodecContext *decoder, const enum AVPixelFormat *formats)
{
	(void)decoder;
	for (; *formats != AV_PIX_FMT_NONE; formats++)
		if (*formats == AV_PIX_FMT_DRM_PRIME)
			return *formats;
	return AV_PIX_FMT_NONE;
}

static void open_input(struct input *in, const char *path)
{
	const AVCodec *codec;
	AVDictionary *options = NULL;

	check_av(avformat_open_input(&in->format, path, NULL, NULL), "open video container");
	in->index = av_find_best_stream(in->format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	check_av(in->index, "find video stream");
	in->video = in->format->streams[in->index];
	in->time_base = in->video->time_base;
	in->sar = in->video->sample_aspect_ratio;
	in->space = in->video->codecpar->color_space;
	in->range = in->video->codecpar->color_range;
	in->decoder = avcodec_alloc_context3(codec);
	in->packet = av_packet_alloc();
	if (!in->decoder || !in->packet)
		fail("cannot allocate video decoder or packet");
	check_av(avcodec_parameters_to_context(in->decoder, in->video->codecpar),
		 "copy video parameters");
	in->decoder->get_format = choose_format;
	in->decoder->thread_count = 1;
	in->decoder->pkt_timebase = in->time_base;
	check_av(av_dict_set(&options, "mv320_tiled", "1", 0), "select tiled VDEC");
	check_av(av_hwdevice_ctx_create(&in->decoder->hw_device_ctx, AV_HWDEVICE_TYPE_V4L2REQUEST,
					NULL, options, 0), "create V4L2 Request device");
	av_dict_free(&options);
	check_av(avcodec_open2(in->decoder, codec, NULL), "open video decoder");
}

static int next_frame(struct input *in, AVFrame *frame)
{
	for (;;) {
		int ret = avcodec_receive_frame(in->decoder, frame);

		if (ret != AVERROR(EAGAIN))
			return ret;
		do {
			av_packet_unref(in->packet);
			ret = av_read_frame(in->format, in->packet);
		} while (ret >= 0 && in->packet->stream_index != in->index);
		if (ret < 0 && ret != AVERROR_EOF)
			return ret;
		ret = avcodec_send_packet(in->decoder, ret == AVERROR_EOF ? NULL : in->packet);
		if (ret < 0)
			return ret;
	}
}

static int seek_input(struct input *in, int64_t target_us)
{
	int64_t target = av_rescale_q(target_us, AV_TIME_BASE_Q, in->time_base) + in->start;
	int ret = av_seek_frame(in->format, in->index, target, AVSEEK_FLAG_BACKWARD);

	if (ret >= 0) {
		av_packet_unref(in->packet);
		avcodec_flush_buffers(in->decoder);
	}
	return ret;
}

static void *decode(void *arg)
{
	struct input *in = arg;
	AVFrame *frame = av_frame_alloc();

	if (!frame)
		fail("cannot allocate decoding frame");
	pthread_mutex_lock(&in->lock);
	while (!in->quit) {
		int ret;

		if (in->seek_pending) {
			int64_t target = in->seek_target;

			in->seek_pending = false;
			pthread_mutex_unlock(&in->lock);
			ret = seek_input(in, target);
			pthread_mutex_lock(&in->lock);
			in->status = ret;
		} else if (in->status || in->count == DECODE_AHEAD) {
			pthread_cond_wait(&in->changed, &in->lock);
			continue;
		} else {
			pthread_mutex_unlock(&in->lock);
			ret = next_frame(in, frame);
			pthread_mutex_lock(&in->lock);
			if (in->quit || in->seek_pending) {
				av_frame_unref(frame);
				continue;
			}
			in->status = ret;
			if (!ret) {
				unsigned int tail = (in->head + in->count) % DECODE_AHEAD;

				av_frame_move_ref(in->queue[tail], frame);
				in->count++;
			}
		}
		pthread_cond_broadcast(&in->changed);
	}
	pthread_mutex_unlock(&in->lock);
	av_frame_free(&frame);
	return NULL;
}

static void start_decoder(struct input *in)
{
	for (int i = 0; i < DECODE_AHEAD; i++) {
		in->queue[i] = av_frame_alloc();
		if (!in->queue[i])
			fail("cannot allocate decode queue frame");
	}
	if (pthread_mutex_init(&in->lock, NULL) || pthread_cond_init(&in->changed, NULL) ||
	    pthread_create(&in->thread, NULL, decode, in))
		fail("cannot start decode thread");
}

static void queue_seek(struct input *in, int64_t target)
{
	pthread_mutex_lock(&in->lock);
	for (int i = 0; i < DECODE_AHEAD; i++)
		av_frame_unref(in->queue[i]);
	in->count = 0;
	in->status = 0;
	in->seek_target = target;
	in->seek_pending = true;
	pthread_cond_broadcast(&in->changed);
	pthread_mutex_unlock(&in->lock);
}

static int get_frame(struct input *in, AVFrame *frame, bool buffering)
{
	int ret = 0;

	pthread_mutex_lock(&in->lock);
	while (!in->status && in->count < (buffering ? DECODE_AHEAD : 1))
		pthread_cond_wait(&in->changed, &in->lock);
	if (in->count) {
		av_frame_move_ref(frame, in->queue[in->head]);
		in->head = (in->head + 1) % DECODE_AHEAD;
		in->count--;
		pthread_cond_broadcast(&in->changed);
	} else {
		ret = in->status;
	}
	pthread_mutex_unlock(&in->lock);
	return ret;
}

static void stop_decoder(struct input *in)
{
	pthread_mutex_lock(&in->lock);
	in->quit = true;
	pthread_cond_broadcast(&in->changed);
	pthread_mutex_unlock(&in->lock);
	pthread_join(in->thread, NULL);
	for (int i = 0; i < DECODE_AHEAD; i++)
		av_frame_free(&in->queue[i]);
	pthread_cond_destroy(&in->changed);
	pthread_mutex_destroy(&in->lock);
}

struct seek { int64_t at, to; };

static int64_t parse_seconds(const char *arg, char **end)
{
	double seconds = strtod(arg, end);

	if (*end == arg || !(seconds >= 0 && seconds <= 86400))
		fail("expected a time from 0 to 86400 seconds");
	return seconds * 1000000 + .5;
}

static void cpu_sample(uint64_t sample[8])
{
	FILE *file = fopen("/proc/stat", "r");

	if (!file)
		fail("cannot open /proc/stat");
	if (fscanf(file, "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
		   " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
		   &sample[0], &sample[1], &sample[2], &sample[3], &sample[4],
		   &sample[5], &sample[6], &sample[7]) != 8)
		fail("cannot read aggregate CPU counters");
	fclose(file);
}

int main(int argc, char **argv)
{
	struct input in = {};
	struct display d = {};
	struct seek seeks[16];
	unsigned int nseeks = 0, seek_index = 0, decoded = 0, dropped = 0, preroll = 0;
	int64_t first = 0, stop_at = -1, discard_until = 0, hold = 0, duration;
	uint64_t cpu_before[8], cpu_after[8], total = 0, started, elapsed, refresh;
	const char *card = "/dev/dri/card0", *path = NULL;
	bool osd = false, eos = false, buffering = true;
	struct rusage usage;
	AVFrame *frame;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help")) {
			puts("Usage: vdp-player [--osd] [--card PATH] [--start SEC] [--stop-at SEC]"
			     " [--seek AT:TO]... [--hold-first SEC] FILE.mp4|FILE.mkv");
			return EXIT_SUCCESS;
		}
		if (!strcmp(argv[i], "--osd")) {
			osd = true;
		} else if (!strcmp(argv[i], "--card") && i + 1 < argc) {
			card = argv[++i];
		} else if ((!strcmp(argv[i], "--start") || !strcmp(argv[i], "--stop-at") ||
			    !strcmp(argv[i], "--hold-first") || !strcmp(argv[i], "--seek")) &&
			   i + 1 < argc) {
			const char *option = argv[i++];
			char *end;
			int64_t time = parse_seconds(argv[i], &end);

			if (!strcmp(option, "--seek")) {
				if (*end != ':' || nseeks == sizeof(seeks) / sizeof(seeks[0]))
					fail("expected --seek AT:TO, at most 16 times");
				seeks[nseeks++] = (struct seek){ time, parse_seconds(end + 1, &end) };
			} else if (!strcmp(option, "--start")) {
				first = time;
			} else if (!strcmp(option, "--stop-at")) {
				stop_at = time;
			} else {
				hold = time;
			}
			if (*end)
				fail("invalid time argument");
		} else if (argv[i][0] != '-' && !path) {
			path = argv[i];
		} else {
			fail("invalid argument, see --help");
		}
	}
	if (!path)
		fail("supply an MP4 or MKV file, see --help");
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, interrupt);
	signal(SIGTERM, interrupt);
	open_input(&in, path);
	open_display(&d, card, osd);
	frame = av_frame_alloc();
	d.current = av_frame_alloc();
	d.pending = av_frame_alloc();
	if (!frame || !d.current || !d.pending)
		fail("cannot allocate frame references");
	in.start = in.video->start_time == AV_NOPTS_VALUE ? 0 : in.video->start_time;
	duration = in.format->duration;
	if (in.video->duration > 0)
		duration = av_rescale_q(in.video->duration, in.time_base, AV_TIME_BASE_Q);
	refresh = 1000000 / d.mode.vrefresh;
	if (first) {
		check_av(seek_input(&in, first), "seek video");
		discard_until = first;
	}
	cpu_sample(cpu_before);
	started = now_us();
	start_decoder(&in);
	while (!stopped) {
		int ret = get_frame(&in, frame, buffering);
		int64_t pts, target;
		int progress = 0;

		if (ret == AVERROR_EOF) {
			eos = true;
			break;
		}
		check_av(ret, "receive decoded frame");
		decoded++;
		if (frame->best_effort_timestamp == AV_NOPTS_VALUE)
			fail("video frame is missing a presentation timestamp");
		pts = av_rescale_q(frame->best_effort_timestamp - in.start, in.time_base, AV_TIME_BASE_Q);
		if (pts < discard_until) {
			preroll++;
			av_frame_unref(frame);
			continue;
		}
		buffering = false;
		complete_flip(&d);
		if (seek_index < nseeks && pts >= seeks[seek_index].at) {
			discard_until = seeks[seek_index++].to;
			av_frame_unref(frame);
			queue_seek(&in, discard_until);
			buffering = true;
			d.segment_shown = 0;
			/* Release the latched reference only after the replacement flip. */
			printf("SEEK %.3f -> %.3f\n", pts / 1e6, discard_until / 1e6);
			continue;
		}
		if (stop_at >= 0 && pts >= stop_at)
			break;
		if (!d.segment_shown) {
			d.base_pts = pts;
			printf("SEGMENT pts=%.3f\n", pts / 1e6);
		} else {
			target = (int64_t)d.segment_us + pts - d.base_pts;
			if (target <= (int64_t)d.last_us + (int64_t)refresh / 2 ||
			    target + (int64_t)refresh < (int64_t)now_us()) {
				dropped++;
				av_frame_unref(frame);
				continue;
			}
			/* Arm the next VT1 shortly before the target refresh. */
			if (target > (int64_t)refresh * 3 / 4)
				sleep_until(target - refresh * 3 / 4);
		}
		if (duration > 0 && pts > 0)
			progress = pts >= duration ? 1000 : av_rescale(pts, 1000, duration);
		if (stopped)
			break;
		submit_frame(&d, frame, &in, progress);
		if (hold) {
			complete_flip(&d);
			printf("HOLD first frame pts=%.3f duration=%.3f\n", pts / 1e6, hold / 1e6);
			sleep_until(now_us() + hold);
			break;
		}
	}
	complete_flip(&d);
	stop_decoder(&in);
	elapsed = now_us() - started;
	cpu_sample(cpu_after);
	getrusage(RUSAGE_SELF, &usage);
	for (int i = 0; i < 8; i++) {
		cpu_after[i] -= cpu_before[i];
		total += cpu_after[i];
	}
	printf("PLAY decoded=%u displayed=%u dropped=%u preroll=%u seeks=%u eos=%d "
	       "missed_vblanks=%u fps=%.3f wall_fps=%.3f seconds=%.3f "
	       "usr=%.2f%% sys=%.2f%% idle=%.2f%% peak_rss_kib=%ld\n",
	       decoded, d.shown, dropped, preroll, seek_index, eos, d.missed,
	       d.shown > 1 ? (d.shown - 1) * 1e6 / (d.last_us - d.first_us) : 0,
	       elapsed ? d.shown * 1e6 / elapsed : 0, elapsed / 1e6,
	       (cpu_after[0] + cpu_after[1]) * 100.0 / (total ? total : 1),
	       (cpu_after[2] + cpu_after[5] + cpu_after[6]) * 100.0 / (total ? total : 1),
	       cpu_after[3] * 100.0 / (total ? total : 1), usage.ru_maxrss);
	if (osd)
		printf("G0 progress flips=%u permille=%d duration=%.3f\n",
		       d.osd_flips, d.progress, duration > 0 ? duration / 1e6 : 0.);
	close_display(&d);
	av_frame_free(&frame);
	avcodec_free_context(&in.decoder);
	av_packet_free(&in.packet);
	avformat_close_input(&in.format);
	return d.shown && seek_index == nseeks ? EXIT_SUCCESS : EXIT_FAILURE;
}
