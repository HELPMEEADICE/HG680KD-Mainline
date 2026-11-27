/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VDP_SCANOUT_TEST_H
#define VDP_SCANOUT_TEST_H

#include <stdbool.h>
#include <stdint.h>

struct vdp_test;

struct vdp_test *vdp_test_open(const char *path, unsigned int width, unsigned int height,
			      bool main10);
void vdp_test_flip(struct vdp_test *test, int video, unsigned int index);
void vdp_test_close(struct vdp_test *test, unsigned int hold_ms);

#endif
