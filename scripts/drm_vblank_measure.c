// SPDX-License-Identifier: GPL-2.0-only
/* Measure the active KMS CRTC's VT1 cadence using the DRM timestamps. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

static int read_tx(void)
{
	static const struct {
		const char *name;
		off_t address;
	} regs[] = {
		{ "hpll", 0xf8a22150 }, { "phy", 0xf8d30098 },
		{ "req", 0xf8ce0068 }, { "ack", 0xf8ce006c }, { "arb", 0xf8ce0070 },
		{ "n", 0xf8ce1048 }, { "sw_cts", 0xf8ce104c }, { "hw_cts", 0xf8ce1050 },
		{ "encoder", 0xf8ce1a60 },
	};
	long page_size = sysconf(_SC_PAGESIZE);
	int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
	unsigned int i;

	if (fd < 0 || page_size <= 0) {
		perror("open /dev/mem");
		if (fd >= 0)
			close(fd);
		return 1;
	}
	for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		off_t offset = regs[i].address % page_size;
		void *page = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd,
				  regs[i].address - offset);

		if (page == MAP_FAILED) {
			perror(regs[i].name);
			close(fd);
			return 1;
		}
		printf("%s=0x%08" PRIx32 "\n", regs[i].name,
		       *(volatile const uint32_t *)((const char *)page + offset));
		munmap(page, page_size);
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	unsigned int first = 0, last = 0;
	int64_t start_us = 0, end_us = 0;
	const unsigned int frames = 121;
	int fd;

	if (argc == 2 && !strcmp(argv[1], "--tx"))
		return read_tx();
	if (argc != 1)
		return 1;
	fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("open DRM");
		return 1;
	}
	for (unsigned int i = 0; i < frames; i++) {
		drmVBlank vblank = {
			.request = { .type = DRM_VBLANK_RELATIVE, .sequence = 1 },
		};
		int ret;

		do {
			ret = drmWaitVBlank(fd, &vblank);
		} while (ret < 0 && errno == EINTR);
		if (ret < 0) {
			perror("DRM_IOCTL_WAIT_VBLANK");
			close(fd);
			return 1;
		}
		if (!i) {
			first = vblank.reply.sequence;
			start_us = (int64_t)vblank.reply.tval_sec * 1000000 + vblank.reply.tval_usec;
		}
		last = vblank.reply.sequence;
		end_us = (int64_t)vblank.reply.tval_sec * 1000000 + vblank.reply.tval_usec;
	}
	close(fd);
	if (end_us <= start_us)
		return 1;
	printf("VT1 waits=%u first=%u last=%u hz=%.6f\n",
	       frames, first, last, (last - first) * 1000000.0 / (end_us - start_us));
	return 0;
}
