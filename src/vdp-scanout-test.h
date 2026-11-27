/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VDP_SCANOUT_TEST_H
#define VDP_SCANOUT_TEST_H

#include <stdbool.h>
#include <stdint.h>

struct vdp_test;
#define VDP_TEST_MAX_CAPTURE 32

struct vdp_test_stats {
	uint32_t frames, first_sequence, last_sequence, missed_vblanks;
	uint64_t first_us, last_us;
};

struct vdp_test *vdp_test_open(const char *path, unsigned int width, unsigned int height,
			      bool main10);
void vdp_test_import(struct vdp_test *test, int video, unsigned int buffers);
int vdp_test_fd(const struct vdp_test *test);
void vdp_test_submit(struct vdp_test *test, unsigned int index);
void vdp_test_complete(struct vdp_test *test);
const struct vdp_test_stats *vdp_test_stats(const struct vdp_test *test);
void vdp_test_close(struct vdp_test *test, unsigned int hold_ms);

#endif
