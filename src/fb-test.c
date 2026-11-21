// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    static const uint32_t colors[] = {
        0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
        0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
    };
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    void *pixels = MAP_FAILED;
    uint32_t *row = NULL;
    uint32_t *check = NULL;
    int result = EXIT_FAILURE;
    int fd = open("/dev/fb0", O_RDWR);

    if (fd < 0) {
        perror("/dev/fb0");
        return result;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        perror("get framebuffer info");
        goto out;
    }
    if (var.bits_per_pixel != 32 || var.red.offset != 16 || var.red.length != 8 ||
        var.green.offset != 8 || var.green.length != 8 || var.blue.offset != 0 ||
        var.blue.length != 8 || var.xoffset || var.yoffset || !var.xres ||
        !var.yres || var.xres > fix.line_length / sizeof(*row) ||
        var.yres > fix.smem_len / fix.line_length) {
        fprintf(stderr, "Unexpected framebuffer layout\n");
        goto out;
    }

    size_t row_size = (size_t)var.xres * sizeof(*row);
    size_t frame_size = (size_t)var.yres * fix.line_length;

    row = malloc(row_size);
    check = malloc(row_size);
    if (!row || !check) {
        perror("allocate test rows");
        goto out;
    }
    for (uint32_t x = 0; x < var.xres; x++)
        row[x] = colors[(uint64_t)x * (sizeof(colors) / sizeof(colors[0])) / var.xres];

    pixels = mmap(NULL, frame_size, PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        perror("map framebuffer");
        goto out;
    }
    /* Dirty framebuffer pages so DRM fbdev can flush the full frame. */
    for (uint32_t y = 0; y < var.yres; y++)
        memcpy((char *)pixels + (size_t)y * fix.line_length, row, row_size);
    if (fsync(fd) < 0) {
        perror("flush framebuffer");
        goto out;
    }
    for (uint32_t y = 0; y < var.yres; y++) {
        off_t offset = (off_t)y * fix.line_length;

        if (pread(fd, check, row_size, offset) != (ssize_t)row_size ||
            memcmp(row, check, row_size)) {
            fprintf(stderr, "Framebuffer readback failed at row %u\n", y);
            goto out;
        }
    }
    printf("fbdev color bars: %ux%u, pitch %u, %u rows read back\n",
           var.xres, var.yres, fix.line_length, var.yres);
    result = EXIT_SUCCESS;

out:
    if (pixels != MAP_FAILED)
        munmap(pixels, (size_t)var.yres * fix.line_length);
    free(check);
    free(row);
    close(fd);
    return result;
}
