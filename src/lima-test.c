/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Offscreen Mali-450 GP (PLBU) + PP (DLBU + WB0) render verification for Lima DRM.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/lima_drm.h>

struct lima_bo {
	uint32_t handle;
	uint32_t va;
	uint64_t offset;
	size_t size;
	void *map;
};

struct lima_pp_wb_reg {
	uint32_t type;
	uint32_t address;
	uint32_t pixel_format;
	uint32_t downsample_factor;
	uint32_t pixel_layout;
	uint32_t pitch;
	uint32_t flags;
	uint32_t mrt_bits;
	uint32_t mrt_pitch;
	uint32_t unused0;
	uint32_t unused1;
	uint32_t unused2;
};

static int bo_create(int fd, size_t size, uint32_t flags, struct lima_bo *bo)
{
	struct drm_lima_gem_create req = { .size = size, .flags = flags };
	struct drm_lima_gem_info info = { 0 };

	if (ioctl(fd, DRM_IOCTL_LIMA_GEM_CREATE, &req) < 0) {
		perror("DRM_IOCTL_LIMA_GEM_CREATE");
		return -1;
	}
	info.handle = req.handle;
	if (ioctl(fd, DRM_IOCTL_LIMA_GEM_INFO, &info) < 0) {
		perror("DRM_IOCTL_LIMA_GEM_INFO");
		return -1;
	}
	bo->handle = req.handle;
	bo->va = info.va;
	bo->offset = info.offset;
	bo->size = size;
	if (!(flags & LIMA_BO_FLAG_HEAP)) {
		bo->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, info.offset);
		if (bo->map == MAP_FAILED) {
			perror("mmap");
			return -1;
		}
		memset(bo->map, 0, size);
	} else {
		bo->map = NULL;
	}
	return 0;
}

static int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int run_gp_plbu_pass(int fd, uint32_t ctx,
			    struct lima_bo *plbu_cmd, struct lima_bo *gp_stream,
			    struct lima_bo *plb, struct lima_bo *heap)
{
	uint32_t *cmd = plbu_cmd->map;
	uint32_t *arr = gp_stream->map;
	struct drm_lima_gp_frame gp_frame;
	struct drm_lima_gem_submit_bo bos[4];
	struct drm_lima_gem_submit submit;
	struct drm_lima_gem_wait wait;
	int i = 0;

	/* 4x4 tiles, 512 bytes per PLB block */
	for (uint32_t j = 0; j < 16; j++)
		arr[j] = plb->va + 512U * j;

	cmd[i++] = 0x00000200U; cmd[i++] = 0x1000010BU; /* UNKNOWN2 */
	cmd[i++] = 0x00000000U; cmd[i++] = 0x1000010CU; /* BLOCK_STEP(0,0,0) */
	cmd[i++] = (3U << 24) | (3U << 8); cmd[i++] = 0x10000109U; /* TILED_DIMENSIONS(4,4) */
	cmd[i++] = 4U; cmd[i++] = 0x30000000U; /* BLOCK_STRIDE(4) */
	cmd[i++] = gp_stream->va; cmd[i++] = 0x28000000U | (16U - 1U) | 1U; /* ARRAY_ADDRESS */
	cmd[i++] = 0x00000000U; cmd[i++] = 0x50000000U; /* END */

	memset(&gp_frame, 0, sizeof(gp_frame));
	gp_frame.frame[0] = 0;
	gp_frame.frame[1] = 0;
	gp_frame.frame[2] = plbu_cmd->va;
	gp_frame.frame[3] = plbu_cmd->va + (uint32_t)(i * 4);
	gp_frame.frame[4] = heap->va;
	gp_frame.frame[5] = heap->va + (uint32_t)heap->size;

	bos[0] = (struct drm_lima_gem_submit_bo){ plbu_cmd->handle, LIMA_SUBMIT_BO_READ };
	bos[1] = (struct drm_lima_gem_submit_bo){ gp_stream->handle, LIMA_SUBMIT_BO_READ };
	bos[2] = (struct drm_lima_gem_submit_bo){ plb->handle, LIMA_SUBMIT_BO_WRITE };
	bos[3] = (struct drm_lima_gem_submit_bo){ heap->handle, LIMA_SUBMIT_BO_WRITE };

	memset(&submit, 0, sizeof(submit));
	submit.ctx = ctx;
	submit.pipe = LIMA_PIPE_GP;
	submit.nr_bos = 4;
	submit.frame_size = sizeof(gp_frame);
	submit.bos = (uintptr_t)bos;
	submit.frame = (uintptr_t)&gp_frame;

	if (ioctl(fd, DRM_IOCTL_LIMA_GEM_SUBMIT, &submit) < 0) {
		perror("GP DRM_IOCTL_LIMA_GEM_SUBMIT");
		return -1;
	}

	memset(&wait, 0, sizeof(wait));
	wait.handle = plb->handle;
	wait.op = LIMA_GEM_WAIT_READ;
	wait.timeout_ns = now_ns() + 2000000000LL;
	if (ioctl(fd, DRM_IOCTL_LIMA_GEM_WAIT, &wait) < 0) {
		perror("GP DRM_IOCTL_LIMA_GEM_WAIT");
		return -1;
	}

	uint32_t *plb_words = plb->map;
	printf("GP PLBU pass complete: plb[0..3]=0x%08x 0x%08x 0x%08x 0x%08x\n",
	       plb_words[0], plb_words[1], plb_words[2], plb_words[3]);
	return 0;
}

static int run_pp_clear_pass(int fd, uint32_t ctx, uint32_t num_pp,
			     struct lima_bo *fb, struct lima_bo *pp_buf,
			     struct lima_bo *plb, struct lima_bo *heap,
			     uint32_t clear_color)
{
	const uint32_t width = 64, height = 64;
	uint32_t *pixels = fb->map;
	struct drm_lima_m450_pp_frame pp;
	struct lima_pp_wb_reg *wb;
	struct drm_lima_gem_submit_bo bos[4];
	struct drm_lima_gem_submit submit;
	struct drm_lima_gem_wait wait;
	uint32_t i, matches = 0;
	int wait_result;

	for (i = 0; i < width * height; i++)
		pixels[i] = 0xdeadbeefU;

	memset(&pp, 0, sizeof(pp));
	/* PP Frame registers per Mesa genxml/common.xml */
	pp.frame[1] = pp_buf->va + 0x100; /* Render Address (pp_frame_rsw) */
	pp.frame[3] = 0x00000002;         /* Early Z */
	pp.frame[4] = 0x00ffffff;         /* Clear Value Depth */
	pp.frame[5] = 0x00000000;         /* Clear Value Stencil */
	pp.frame[6] = clear_color;        /* Clear Value 8bpc Color 0..3 */
	pp.frame[7] = clear_color;
	pp.frame[8] = clear_color;
	pp.frame[9] = clear_color;
	pp.frame[10] = width - 1;         /* Bounding Box Right */
	pp.frame[11] = height - 1;        /* Bounding Box Bottom */
	pp.frame[16] = 1;                 /* Origin X */
	pp.frame[17] = height * 2 - 1;    /* Origin Y */
	pp.frame[18] = 0x77;              /* Subpixel Specifier */
	pp.frame[19] = 1;                 /* Tiebreak Mode */
	pp.frame[21] = 0x0e0c;            /* Scale/Flip flags */
	pp.frame[22] = 0x8888;            /* Tilebuffer Channel Layout 8888 */

	wb = (struct lima_pp_wb_reg *)pp.wb;
	wb[0].type = 2;                   /* Color buffer writeback */
	wb[0].address = fb->va;
	wb[0].pixel_format = 3;           /* B8G8R8A8 */
	wb[0].downsample_factor = 0;
	wb[0].pixel_layout = 0;           /* Linear */
	wb[0].pitch = (width * 4) / 8;    /* 8-byte units */
	wb[0].flags = 0x4;                /* Swap R/B -> RGBA8888 matches clear_color */

	pp.num_pp = num_pp;
	pp.use_dlbu = 1;
	pp.dlbu_regs[0] = plb->va;
	pp.dlbu_regs[1] = ((4U - 1U) << 16) | (4U - 1U);
	pp.dlbu_regs[2] = (2U << 28);     /* log2(512) - 7 = 2, shift_h=0, shift_w=0 */
	pp.dlbu_regs[3] = ((4U - 1U) << 24) | ((4U - 1U) << 16);

	bos[0] = (struct drm_lima_gem_submit_bo){ fb->handle, LIMA_SUBMIT_BO_WRITE };
	bos[1] = (struct drm_lima_gem_submit_bo){ pp_buf->handle, LIMA_SUBMIT_BO_READ };
	bos[2] = (struct drm_lima_gem_submit_bo){ plb->handle, LIMA_SUBMIT_BO_READ };
	bos[3] = (struct drm_lima_gem_submit_bo){ heap->handle, LIMA_SUBMIT_BO_READ };

	memset(&submit, 0, sizeof(submit));
	submit.ctx = ctx;
	submit.pipe = LIMA_PIPE_PP;
	submit.nr_bos = 4;
	submit.frame_size = sizeof(pp);
	submit.bos = (uintptr_t)bos;
	submit.frame = (uintptr_t)&pp;

	if (ioctl(fd, DRM_IOCTL_LIMA_GEM_SUBMIT, &submit) < 0) {
		perror("PP DRM_IOCTL_LIMA_GEM_SUBMIT");
		return -1;
	}

	memset(&wait, 0, sizeof(wait));
	wait.handle = fb->handle;
	wait.op = LIMA_GEM_WAIT_READ;
	wait.timeout_ns = now_ns() + 2000000000LL;
	wait_result = ioctl(fd, DRM_IOCTL_LIMA_GEM_WAIT, &wait);
	if (wait_result < 0)
		perror("PP DRM_IOCTL_LIMA_GEM_WAIT");

	for (i = 0; i < width * height; i++) {
		if (pixels[i] == clear_color)
			matches++;
	}

	printf("PP DLBU clear 0x%08x: %u/%u pixels matched (first=0x%08x mid=0x%08x last=0x%08x)\n",
	       clear_color, matches, width * height,
	       pixels[0], pixels[(height / 2) * width + width / 2],
	       pixels[width * height - 1]);
	return wait_result == 0 && matches == width * height ? 0 : -1;
}

int main(int argc, char **argv)
{
	static const uint32_t pp_clear_program[8] = {
		0x00020425, 0x0000000c, 0x01e007cf, 0xb0000000,
		0x000005f5, 0x00000000, 0x00000000, 0x00000000,
	};
	const char *path = "/dev/dri/renderD128";
	char name[64] = { 0 }, date[64] = { 0 }, desc[128] = { 0 };
	struct drm_version ver = {
		.name_len = sizeof(name) - 1, .name = name,
		.date_len = sizeof(date) - 1, .date = date,
		.desc_len = sizeof(desc) - 1, .desc = desc,
	};
	struct drm_lima_get_param gp = { 0 };
	struct drm_lima_ctx_create ctx = { 0 };
	struct lima_bo fb, pp_buf, plb, plbu_cmd, gp_stream, heap;
	uint32_t num_pp, gpu_id, gp_ver, pp_ver;
	unsigned long cores = 0, repeat = 2;
	int alternate = 0;
	uint32_t *rsw;
	int fd, opt;

	while ((opt = getopt(argc, argv, "p:n:a")) != -1) {
		char *end;
		unsigned long value;

		if (opt == 'a') {
			alternate = 1;
			continue;
		}
		if (opt != 'p' && opt != 'n')
			goto usage;
		value = strtoul(optarg, &end, 10);
		if (*end || !value || value > (opt == 'p' ? 8 : 10000))
			goto usage;
		if (opt == 'p')
			cores = value;
		else
			repeat = value;
	}
	if (optind != argc || (alternate && cores))
		goto usage;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror("open /dev/dri/renderD128");
		return 1;
	}

	if (ioctl(fd, DRM_IOCTL_VERSION, &ver) < 0) {
		perror("DRM_IOCTL_VERSION");
		return 1;
	}
	printf("DRM device %s: driver=%s version=%d.%d.%d desc=%s\n",
	       path, name, ver.version_major, ver.version_minor,
	       ver.version_patchlevel, desc);

	gp.param = DRM_LIMA_PARAM_GPU_ID;
	if (ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp) < 0) {
		perror("GET_PARAM GPU_ID");
		return 1;
	}
	gpu_id = (uint32_t)gp.value;

	gp.param = DRM_LIMA_PARAM_NUM_PP;
	ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp);
	num_pp = (uint32_t)gp.value;

	gp.param = DRM_LIMA_PARAM_GP_VERSION;
	ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp);
	gp_ver = (uint32_t)gp.value;

	gp.param = DRM_LIMA_PARAM_PP_VERSION;
	ioctl(fd, DRM_IOCTL_LIMA_GET_PARAM, &gp);
	pp_ver = (uint32_t)gp.value;

	printf("Lima params: gpu_id=%u num_pp=%u gp_ver=0x%08x pp_ver=0x%08x\n",
	       gpu_id, num_pp, gp_ver, pp_ver);

	if (gpu_id != DRM_LIMA_PARAM_GPU_ID_MALI450 || num_pp == 0 || num_pp > 8) {
		fprintf(stderr, "Unexpected Mali-450 params\n");
		return 1;
	}
	if (cores > num_pp || (alternate && num_pp < 2)) {
		fprintf(stderr, "Requested PP count exceeds the available cores\n");
		return 1;
	}
	if (!cores)
		cores = num_pp;

	if (ioctl(fd, DRM_IOCTL_LIMA_CTX_CREATE, &ctx) < 0) {
		perror("DRM_IOCTL_LIMA_CTX_CREATE");
		return 1;
	}
	printf("Created Lima context id=%u\n", ctx.id);

	if (bo_create(fd, 64 * 64 * 4, 0, &fb) < 0 ||
	    bo_create(fd, 4096, 0, &pp_buf) < 0 ||
	    bo_create(fd, 8192, 0, &plb) < 0 ||
	    bo_create(fd, 4096, 0, &plbu_cmd) < 0 ||
	    bo_create(fd, 4096, 0, &gp_stream) < 0 ||
	    bo_create(fd, 65536, LIMA_BO_FLAG_HEAP, &heap) < 0)
		return 1;

	printf("GEM BOs: fb=0x%08x pp_buf=0x%08x plb=0x%08x plbu_cmd=0x%08x gp_stream=0x%08x heap=0x%08x\n",
	       fb.va, pp_buf.va, plb.va, plbu_cmd.va, gp_stream.va, heap.va);

	/* Populate clear shader + static frame RSW */
	memcpy(pp_buf.map, pp_clear_program, sizeof(pp_clear_program));
	rsw = (uint32_t *)((uint8_t *)pp_buf.map + 0x100);
	rsw[8] = 0x0000f008;
	rsw[9] = pp_buf.va;
	rsw[13] = 0x00000100;

	for (unsigned long i = 0; i < repeat; i++) {
		uint32_t active_pp = alternate && !(i % 2) ? 1 : cores;
		uint32_t color = i % 2 ? 0xffaabbccU : 0xff336699U;

		printf("Job %lu: cores=%u\n", i + 1, active_pp);
		if (run_gp_plbu_pass(fd, ctx.id, &plbu_cmd, &gp_stream, &plb, &heap) < 0 ||
		    run_pp_clear_pass(fd, ctx.id, active_pp, &fb, &pp_buf, &plb, &heap, color) < 0)
			return 1;
	}

	printf("LIMA GP+PP OFFSCREEN RENDER TEST: PASS\n");
	close(fd);
	return 0;

usage:
	fprintf(stderr, "Usage: %s [-p cores | -a] [-n jobs]\n", argv[0]);
	return 2;
}
