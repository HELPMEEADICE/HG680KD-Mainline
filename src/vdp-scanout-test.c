// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>

#include "hi3798mv320-tiled.h"
#include "vdp-scanout-test.h"

#define V0_FB_PROP 3

struct vdp_test {
	int fd;
	uint32_t format, width, height, blob;
	struct vdp_test_stats stats;
	uint32_t fb[VDP_TEST_MAX_CAPTURE];
	uint32_t objects[4], counts[4], props[27];
	uint64_t values[27];
	uint32_t overlay_fb[2], overlay_handles[2];
	unsigned int g0_prop;
	bool overlay;
};

static void drm_fail(const char *reason)
{
	fprintf(stderr, "%s\n", reason);
	exit(EXIT_FAILURE);
}

static void drm_call(int fd, unsigned long cmd, void *arg, const char *name)
{
	int ret;

	do {
		ret = ioctl(fd, cmd, arg);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		perror(name);
		exit(EXIT_FAILURE);
	}
}

#define CALL(fd, cmd, arg) drm_call(fd, cmd, arg, #cmd)
#define PTR(p) ((uintptr_t)(p))

static uint32_t find_property(int fd, uint32_t id, uint32_t type, const char *name, uint64_t *val)
{
	uint32_t props[64];
	uint64_t values[64];
	struct drm_mode_obj_get_properties obj = {
		.obj_id = id, .obj_type = type, .count_props = 64,
		.props_ptr = PTR(props), .prop_values_ptr = PTR(values),
	};
	unsigned int i;

	CALL(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &obj);
	if (obj.count_props > 64)
		drm_fail("too many DRM properties");
	for (i = 0; i < obj.count_props; i++) {
		struct drm_mode_get_property p = { .prop_id = props[i] };

		CALL(fd, DRM_IOCTL_MODE_GETPROPERTY, &p);
		if (!strcmp(p.name, name)) {
			if (val)
				*val = values[i];
			return props[i];
		}
	}
	return 0;
}

static uint32_t property(int fd, uint32_t id, uint32_t type, const char *name, uint64_t *val)
{
	uint32_t prop = find_property(fd, id, type, name, val);

	if (!prop)
		drm_fail(name);
	return prop;
}

static bool supports_format(int fd, uint32_t plane, uint32_t format, uint64_t modifier)
{
	struct drm_mode_get_blob get = {};
	struct drm_format_modifier_blob *blob;
	struct drm_format_modifier *mods;
	uint32_t *formats;
	uint64_t id;
	unsigned int i, j;
	bool found = false;

	property(fd, plane, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &id);
	get.blob_id = id;
	CALL(fd, DRM_IOCTL_MODE_GETPROPBLOB, &get);
	blob = malloc(get.length);
	if (!blob || get.length < sizeof(*blob))
		drm_fail("invalid IN_FORMATS blob length");
	get.data = PTR(blob);
	CALL(fd, DRM_IOCTL_MODE_GETPROPBLOB, &get);
	if (blob->formats_offset > get.length || blob->modifiers_offset > get.length ||
	    blob->count_formats > (get.length - blob->formats_offset) / sizeof(*formats) ||
	    blob->count_modifiers > (get.length - blob->modifiers_offset) / sizeof(*mods))
		drm_fail("invalid IN_FORMATS offsets");
	formats = (void *)((char *)blob + blob->formats_offset);
	mods = (void *)((char *)blob + blob->modifiers_offset);
	for (i = 0; i < blob->count_modifiers; i++) {
		if (mods[i].modifier != modifier)
			continue;
		for (j = 0; j < 64 && (uint64_t)mods[i].offset + j < blob->count_formats; j++)
			if ((mods[i].formats & (1ULL << j)) && formats[mods[i].offset + j] == format)
				found = true;
	}
	free(blob);
	return found;
}

static uint64_t enum_value(int fd, uint32_t id, const char *name)
{
	struct drm_mode_property_enum items[16];
	struct drm_mode_get_property p = {
		.prop_id = id, .count_enum_blobs = 16, .enum_blob_ptr = PTR(items),
	};
	unsigned int i;

	CALL(fd, DRM_IOCTL_MODE_GETPROPERTY, &p);
	for (i = 0; i < p.count_enum_blobs && i < 16; i++)
		if (!strcmp(items[i].name, name))
			return items[i].value;
	drm_fail(name);
	return 0;
}

static void create_overlay(struct vdp_test *t, unsigned int index, uint32_t format,
			   unsigned int width, unsigned int height)
{
	struct drm_mode_create_dumb dumb = { .width = width, .height = height, .bpp = 32 };
	struct drm_mode_map_dumb map = {};
	struct drm_mode_fb_cmd2 fb = { .width = width, .height = height, .pixel_format = format };
	uint8_t *pixels;
	unsigned int x, y;

	CALL(t->fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
	fb.handles[0] = map.handle = dumb.handle;
	fb.pitches[0] = dumb.pitch;
	CALL(t->fd, DRM_IOCTL_MODE_ADDFB2, &fb);
	CALL(t->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, t->fd, map.offset);
	if (pixels == MAP_FAILED)
		drm_fail("cannot map the G0 dumb buffer");
	for (y = 0; y < height; y++) {
		uint32_t *line = (void *)(pixels + y * dumb.pitch);

		for (x = 0; x < width; x++) {
			if (x < 8 || y < 8 || x >= width - 8 || y >= height - 8)
				line[x] = 0xfff0e040;
			else if (y < height / 3)
				line[x] = 0x002090f0;
			else if (x >= width * 3 / 4)
				line[x] = index ? 0xffe02040 : 0xff2090f0;
			else
				line[x] = 0x80000000;
		}
	}
	munmap(pixels, dumb.size);
	t->overlay_fb[index] = fb.fb_id;
	t->overlay_handles[index] = dumb.handle;
}

struct vdp_test *vdp_test_open(const char *path, unsigned int width, unsigned int height,
			      bool main10)
{
	static const char * const plane_props[] = {
		"FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
		"CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H",
	};
	struct vdp_test *t = calloc(1, sizeof(*t));
	struct drm_set_client_cap atomic = { .capability = DRM_CLIENT_CAP_ATOMIC, .value = 1 };
	struct drm_mode_card_res res = {};
	struct drm_mode_get_plane_res planes = {};
	struct drm_mode_get_connector conn = {};
	struct drm_mode_modeinfo mode = {};
	struct drm_mode_create_blob blob = { .data = PTR(&mode), .length = sizeof(mode) };
	const char *overlay = getenv("VDP_TEST_OVERLAY");
	const char *encoding = getenv("VDP_TEST_ENCODING");
	const char *range = getenv("VDP_TEST_RANGE");
	bool scale = getenv("VDP_TEST_SCALE") != NULL;
	uint32_t plane_ids[16], gfx_format = DRM_FORMAT_ARGB8888;
	unsigned int i;

	if (!t)
		drm_fail("cannot allocate display state");
	t->fd = open(path, O_RDWR | O_CLOEXEC);
	if (t->fd < 0) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	t->width = width;
	t->height = height;
	t->format = main10 ? DRM_FORMAT_HI3798MV320_TILED10 : DRM_FORMAT_NV21;
	CALL(t->fd, DRM_IOCTL_SET_CLIENT_CAP, &atomic);
	CALL(t->fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
	if (res.count_connectors != 1 || res.count_crtcs != 1)
		drm_fail("expected one connector and one CRTC");
	res.connector_id_ptr = PTR(&t->objects[0]);
	res.crtc_id_ptr = PTR(&t->objects[1]);
	res.count_fbs = res.count_encoders = 0;
	CALL(t->fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
	CALL(t->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes);
	if (!planes.count_planes || planes.count_planes > 16)
		drm_fail("invalid DRM plane count");
	planes.plane_id_ptr = PTR(plane_ids);
	CALL(t->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes);
	if (overlay && !strcmp(overlay, "xrgb"))
		gfx_format = DRM_FORMAT_XRGB8888;
	else if (overlay && strcmp(overlay, "argb"))
		drm_fail("VDP_TEST_OVERLAY expects argb or xrgb");
	for (i = 0; i < planes.count_planes; i++) {
		if (supports_format(t->fd, plane_ids[i], t->format, MV320_TILED_MODIFIER))
			t->objects[2] = plane_ids[i];
		if (overlay && supports_format(t->fd, plane_ids[i], gfx_format, DRM_FORMAT_MOD_LINEAR))
			t->objects[3] = plane_ids[i];
	}
	if (!t->objects[2])
		drm_fail("V0 IN_FORMATS lacks the selected tiled format");
	if (overlay && !t->objects[3])
		drm_fail("G0 IN_FORMATS lacks the selected linear format");
	conn.connector_id = t->objects[0];
	CALL(t->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
	if (conn.count_modes != 1)
		drm_fail("expected one active firmware mode");
	conn.modes_ptr = PTR(&mode);
	conn.count_props = conn.count_encoders = 0;
	CALL(t->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
	if (!scale && (width > mode.hdisplay || height > mode.vdisplay))
		drm_fail("fixture is larger than the firmware mode");
	CALL(t->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob);
	t->blob = blob.blob_id;
	t->counts[0] = 1;
	t->counts[1] = 2;
	t->counts[2] = sizeof(plane_props) / sizeof(*plane_props);
	t->props[0] = property(t->fd, t->objects[0], DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
	t->props[1] = property(t->fd, t->objects[1], DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
	t->props[2] = property(t->fd, t->objects[1], DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
	for (i = 0; i < t->counts[2]; i++)
		t->props[3 + i] = property(t->fd, t->objects[2], DRM_MODE_OBJECT_PLANE, plane_props[i], NULL);
	t->values[0] = t->values[4] = t->objects[1];
	t->values[1] = t->blob;
	t->values[2] = 1;
	t->values[7] = width << 16;
	t->values[8] = height << 16;
	t->values[9] = scale ? 0 : ((mode.hdisplay - width) / 2) & ~1U;
	t->values[10] = scale ? 0 : ((mode.vdisplay - height) / 2) & ~1U;
	t->values[11] = scale ? mode.hdisplay : width;
	t->values[12] = scale ? mode.vdisplay : height;
	i = V0_FB_PROP + t->counts[2];
	t->props[i] = find_property(t->fd, t->objects[2], DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING", NULL);
	if (t->props[i]) {
		t->values[i] = enum_value(t->fd, t->props[i], encoding ? encoding : "ITU-R BT.709 YCbCr");
		t->counts[2]++;
	} else if (encoding) {
		drm_fail("V0 has no COLOR_ENCODING property");
	}
	i = V0_FB_PROP + t->counts[2];
	t->props[i] = find_property(t->fd, t->objects[2], DRM_MODE_OBJECT_PLANE, "COLOR_RANGE", NULL);
	if (t->props[i]) {
		t->values[i] = enum_value(t->fd, t->props[i], range ? range : "YCbCr limited range");
		t->counts[2]++;
	} else if (range) {
		drm_fail("V0 has no COLOR_RANGE property");
	}
	if (overlay) {
		unsigned int factor = mode.vdisplay == 2160 ? 2 : 1;
		unsigned int w = mode.hdisplay / 2, h = mode.vdisplay / (8 * factor) * factor;
		unsigned int g0 = t->objects[3];
		uint32_t alpha = 65535;
		const char *alpha_env = getenv("VDP_TEST_ALPHA");

		t->overlay = true;
		t->g0_prop = V0_FB_PROP + t->counts[2];
		if (alpha_env) {
			char *end;
			unsigned long value = strtoul(alpha_env, &end, 10);

			if (end == alpha_env || *end || value > 65535)
				drm_fail("VDP_TEST_ALPHA expects an integer from 0 to 65535");
			alpha = value;
		}
		for (i = 0; i < sizeof(plane_props) / sizeof(*plane_props); i++)
			t->props[t->g0_prop + i] = property(t->fd, g0, DRM_MODE_OBJECT_PLANE, plane_props[i], NULL);
		t->counts[3] = i + 2;
		t->props[t->g0_prop + 10] = property(t->fd, g0, DRM_MODE_OBJECT_PLANE, "pixel blend mode", NULL);
		t->values[t->g0_prop + 10] = enum_value(t->fd, t->props[t->g0_prop + 10], "Coverage");
		t->props[t->g0_prop + 11] = property(t->fd, g0, DRM_MODE_OBJECT_PLANE, "alpha", NULL);
		t->values[t->g0_prop + 11] = alpha;
		t->values[t->g0_prop + 1] = t->objects[1];
		t->values[t->g0_prop + 4] = (uint64_t)(w / factor) << 16;
		t->values[t->g0_prop + 5] = (uint64_t)(h / factor) << 16;
		t->values[t->g0_prop + 6] = (mode.hdisplay - w) / 2;
		t->values[t->g0_prop + 7] = mode.vdisplay - 2 * h;
		t->values[t->g0_prop + 8] = w;
		t->values[t->g0_prop + 9] = h;
		for (i = 0; i < 2; i++)
			create_overlay(t, i, gfx_format, w / factor, h / factor);
		printf("G0 plane=%u format=%s source=%ux%u display=%ux%u alpha=%u\n",
		       g0, overlay, w / factor, h / factor, w, h, alpha);
	}
	printf("DRM connector=%u CRTC=%u V0=%u firmware=%s refresh=%u modifier=%#llx\n",
	       t->objects[0], t->objects[1], t->objects[2], mode.name, mode.vrefresh,
	       MV320_TILED_MODIFIER);
	return t;
}

static uint32_t add_fb(struct vdp_test *t, int video, unsigned int index)
{
	struct v4l2_exportbuffer exp = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.index = index, .flags = O_CLOEXEC | O_RDWR };
	struct drm_prime_handle prime = {};
	struct drm_gem_close close_handle = {};
	unsigned int stride = (t->width + 255) & ~255U;
	unsigned int y = stride * ((t->height + 63) & ~63U), i;
	struct drm_mode_fb_cmd2 fb = {
		.width = t->width, .height = t->height, .pixel_format = t->format,
		.flags = DRM_MODE_FB_MODIFIERS, .offsets = { 0, y, 0, 0 },
		.pitches = { stride, stride, 0, 0 },
	};
	unsigned int count = 2;

	if (t->format == DRM_FORMAT_HI3798MV320_TILED10) {
		fb.offsets[2] = 3 * y / 2;
		fb.offsets[3] = 7 * y / 4;
		fb.pitches[2] = fb.pitches[3] = stride / 4;
		count = 4;
	}
	CALL(video, VIDIOC_EXPBUF, &exp);
	prime.fd = exp.fd;
	CALL(t->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime);
	close(exp.fd);
	for (i = 0; i < count; i++) {
		fb.handles[i] = prime.handle;
		fb.modifier[i] = MV320_TILED_MODIFIER;
	}
	CALL(t->fd, DRM_IOCTL_MODE_ADDFB2, &fb);
	close_handle.handle = prime.handle;
	CALL(t->fd, DRM_IOCTL_GEM_CLOSE, &close_handle);
	return fb.fb_id;
}

void vdp_test_import(struct vdp_test *t, int video, unsigned int buffers)
{
	unsigned int i;

	if (buffers > VDP_TEST_MAX_CAPTURE)
		drm_fail("CAPTURE count exceeds display buffer table");
	for (i = 0; i < buffers; i++)
		t->fb[i] = add_fb(t, video, i);
}

int vdp_test_fd(const struct vdp_test *t)
{
	return t->fd;
}

static void commit(struct vdp_test *t, unsigned int flags, bool flip_only)
{
	uint32_t flip_counts[] = { 1, 1 };
	uint32_t flip_props[] = { t->props[V0_FB_PROP], t->props[t->g0_prop] };
	uint64_t flip_values[] = { t->values[V0_FB_PROP], t->values[t->g0_prop] };
	struct drm_mode_atomic cmd = {
		.flags = flags, .count_objs = 3 + t->overlay, .objs_ptr = PTR(t->objects),
		.count_props_ptr = PTR(t->counts), .props_ptr = PTR(t->props),
		.prop_values_ptr = PTR(t->values), .user_data = t->stats.frames + 1,
	};

	if (flip_only) {
		cmd.count_objs = 1 + t->overlay;
		cmd.objs_ptr = PTR(&t->objects[2]);
		cmd.count_props_ptr = PTR(flip_counts);
		cmd.props_ptr = PTR(flip_props);
		cmd.prop_values_ptr = PTR(flip_values);
	}
	CALL(t->fd, DRM_IOCTL_MODE_ATOMIC, &cmd);
}

void vdp_test_submit(struct vdp_test *t, unsigned int index)
{
	unsigned int flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;

	if (index >= VDP_TEST_MAX_CAPTURE || !t->fb[index])
		drm_fail("CAPTURE index has no framebuffer");
	t->values[V0_FB_PROP] = t->fb[index];
	if (t->overlay)
		t->values[t->g0_prop] = t->overlay_fb[t->stats.frames % 2];
	if (!t->stats.frames)
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	commit(t, flags, t->stats.frames != 0);
}

static struct drm_event_vblank read_flip(struct vdp_test *t, uint64_t user_data)
{
	struct drm_event_vblank event;
	ssize_t ret;

	do {
		ret = read(t->fd, &event, sizeof(event));
	} while (ret < 0 && errno == EINTR);
	if (ret != sizeof(event) || event.base.length != sizeof(event) ||
	    event.base.type != DRM_EVENT_FLIP_COMPLETE || event.crtc_id != t->objects[1] ||
	    event.user_data != user_data)
		drm_fail("missing or invalid DRM page-flip completion");
	return event;
}

void vdp_test_complete(struct vdp_test *t)
{
	struct vdp_test_stats *stats = &t->stats;
	struct drm_event_vblank event = read_flip(t, stats->frames + 1);

	if (stats->frames && (int32_t)(event.sequence - stats->last_sequence) <= 0)
		drm_fail("V0 page-flip sequence did not advance");

	stats->last_us = (uint64_t)event.tv_sec * 1000000 + event.tv_usec;
	if (!stats->frames) {
		stats->first_us = stats->last_us;
		stats->first_sequence = event.sequence;
	} else {
		stats->missed_vblanks += event.sequence - stats->last_sequence - 1;
	}
	stats->last_sequence = event.sequence;
	stats->frames++;
}

const struct vdp_test_stats *vdp_test_stats(const struct vdp_test *t)
{
	return &t->stats;
}

void vdp_test_close(struct vdp_test *t, unsigned int hold_ms)
{
	struct drm_mode_destroy_blob blob = { .blob_id = t->blob };
	unsigned int i;

	if (t->overlay && t->stats.frames && getenv("VDP_TEST_G0_ONLY")) {
		uint32_t count = 1;
		uint64_t fb = t->overlay_fb[t->stats.frames % 2];
		struct drm_mode_atomic cmd = {
			.flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
			.count_objs = 1, .objs_ptr = PTR(&t->objects[3]),
			.count_props_ptr = PTR(&count), .props_ptr = PTR(&t->props[t->g0_prop]),
			.prop_values_ptr = PTR(&fb), .user_data = t->stats.frames + 1,
		};
		struct drm_event_vblank event;

		CALL(t->fd, DRM_IOCTL_MODE_ATOMIC, &cmd);
		event = read_flip(t, cmd.user_data);
		if ((int32_t)(event.sequence - t->stats.last_sequence) <= 0)
			drm_fail("G0-only page-flip sequence did not advance");
		printf("G0 independent flip complete vblank=%u\n", event.sequence);
	}

	printf("V0 complete=%u first_vblank=%u last_vblank=%u\n",
	       t->stats.frames, t->stats.first_sequence, t->stats.last_sequence);
	fflush(stdout);
	if (hold_ms) {
		struct timespec wait = { .tv_sec = hold_ms / 1000, .tv_nsec = (hold_ms % 1000) * 1000000UL };

		while (nanosleep(&wait, &wait) && errno == EINTR)
			;
	}
	t->values[0] = t->values[1] = t->values[2] = t->values[3] = t->values[4] = 0;
	if (t->overlay)
		t->values[t->g0_prop] = t->values[t->g0_prop + 1] = 0;
	commit(t, DRM_MODE_ATOMIC_ALLOW_MODESET, false);
	for (i = 0; i < VDP_TEST_MAX_CAPTURE; i++)
		if (t->fb[i])
			CALL(t->fd, DRM_IOCTL_MODE_RMFB, &t->fb[i]);
	for (i = 0; i < 2 && t->overlay; i++) {
		struct drm_mode_destroy_dumb dumb = { .handle = t->overlay_handles[i] };

		CALL(t->fd, DRM_IOCTL_MODE_RMFB, &t->overlay_fb[i]);
		CALL(t->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb);
	}
	CALL(t->fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &blob);
	close(t->fd);
	free(t);
}
