// SPDX-License-Identifier: GPL-2.0-only
/* Keep the existing KMS CRTC's VT1 IRQ observable during a static test image. */
#include <errno.h>
#include <fcntl.h>
#include <libdrm/drm.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
	int fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC);
	unsigned int first = 0, last = 0;
	const unsigned int frames = 18 * 60;

	if (fd < 0) {
		perror("open DRM");
		return 1;
	}
	for (unsigned int i = 0; i < frames; i++) {
		drm_wait_vblank_t vblank = {
			.request = { .type = _DRM_VBLANK_RELATIVE, .sequence = 1 },
		};
		int ret;

		do {
			ret = ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vblank);
		} while (ret < 0 && errno == EINTR);
		if (ret < 0) {
			perror("DRM_IOCTL_WAIT_VBLANK");
			close(fd);
			return 1;
		}
		if (!i) {
			first = vblank.reply.sequence;
			puts("VT1 ready");
			fflush(stdout);
		}
		last = vblank.reply.sequence;
	}
	printf("VT1 waits=%u first=%u last=%u\n", frames, first, last);
	close(fd);
	return 0;
}
