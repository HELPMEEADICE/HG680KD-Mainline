// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>

#include "hi3798mv320-tiled.h"
#include "vdp-scanout-test.h"

struct vdp_test {
	int fd;
	uint32_t format, width, height, blob;
	struct vdp_test_stats stats;
	uint32_t fb[VDP_TEST_MAX_CAPTURE];
	uint32_t objects[3], counts[3], props[13];
	uint64_t values[13];
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

static uint32_t property(int fd, uint32_t id, uint32_t type, const char *name, uint64_t *val)
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
	drm_fail(name);
	return 0;
}

static bool supports_format(int fd, uint32_t plane, uint32_t format)
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
		if (mods[i].modifier != MV320_TILED_MODIFIER)
			continue;
		for (j = 0; j < 64 && (uint64_t)mods[i].offset + j < blob->count_formats; j++)
			if ((mods[i].formats & (1ULL << j)) && formats[mods[i].offset + j] == format)
				found = true;
	}
	free(blob);
	return found;
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
	if (planes.count_planes != 1)
		drm_fail("expected one V0 plane");
	planes.plane_id_ptr = PTR(&t->objects[2]);
	CALL(t->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes);
	if (!supports_format(t->fd, t->objects[2], t->format))
		drm_fail("V0 IN_FORMATS lacks the selected tiled format");
	conn.connector_id = t->objects[0];
	CALL(t->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
	if (conn.count_modes != 1)
		drm_fail("expected one active firmware mode");
	conn.modes_ptr = PTR(&mode);
	conn.count_props = conn.count_encoders = 0;
	CALL(t->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
	if (width > mode.hdisplay || height > mode.vdisplay)
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
	t->values[9] = ((mode.hdisplay - width) / 2) & ~1U;
	t->values[10] = ((mode.vdisplay - height) / 2) & ~1U;
	t->values[11] = width;
	t->values[12] = height;
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
	uint32_t one = 1;
	struct drm_mode_atomic cmd = {
		.flags = flags, .count_objs = 3, .objs_ptr = PTR(t->objects),
		.count_props_ptr = PTR(t->counts), .props_ptr = PTR(t->props),
		.prop_values_ptr = PTR(t->values), .user_data = t->stats.frames + 1,
	};

	if (flip_only) {
		cmd.count_objs = 1;
		cmd.objs_ptr = PTR(&t->objects[2]);
		cmd.count_props_ptr = PTR(&one);
		cmd.props_ptr = PTR(&t->props[3]);
		cmd.prop_values_ptr = PTR(&t->values[3]);
	}
	CALL(t->fd, DRM_IOCTL_MODE_ATOMIC, &cmd);
}

void vdp_test_submit(struct vdp_test *t, unsigned int index)
{
	unsigned int flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;

	if (index >= VDP_TEST_MAX_CAPTURE || !t->fb[index])
		drm_fail("CAPTURE index has no framebuffer");
	t->values[3] = t->fb[index];
	if (!t->stats.frames)
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	commit(t, flags, t->stats.frames != 0);
}

void vdp_test_complete(struct vdp_test *t)
{
	struct vdp_test_stats *stats = &t->stats;
	struct drm_event_vblank event;
	ssize_t ret;

	do {
		ret = read(t->fd, &event, sizeof(event));
	} while (ret < 0 && errno == EINTR);
	if (ret != sizeof(event) || event.base.length != sizeof(event) ||
	    event.base.type != DRM_EVENT_FLIP_COMPLETE || event.crtc_id != t->objects[1] ||
	    event.user_data != stats->frames + 1 ||
	    (stats->frames && (int32_t)(event.sequence - stats->last_sequence) <= 0))
		drm_fail("missing or invalid V0 page-flip completion");
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

	printf("V0 complete=%u first_vblank=%u last_vblank=%u\n",
	       t->stats.frames, t->stats.first_sequence, t->stats.last_sequence);
	fflush(stdout);
	if (hold_ms) {
		struct timespec wait = { .tv_sec = hold_ms / 1000, .tv_nsec = (hold_ms % 1000) * 1000000UL };

		while (nanosleep(&wait, &wait) && errno == EINTR)
			;
	}
	t->values[0] = t->values[1] = t->values[2] = t->values[3] = t->values[4] = 0;
	commit(t, DRM_MODE_ATOMIC_ALLOW_MODESET, false);
	for (i = 0; i < VDP_TEST_MAX_CAPTURE; i++)
		if (t->fb[i])
			CALL(t->fd, DRM_IOCTL_MODE_RMFB, &t->fb[i]);
	CALL(t->fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &blob);
	close(t->fd);
	free(t);
}
