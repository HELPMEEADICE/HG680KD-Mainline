// SPDX-License-Identifier: GPL-2.0-only
/* Run full-picture MPEG requests and write tightly packed NV21 if requested. */

#include <errno.h>
#include <fcntl.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "hi3798mv320-tiled.h"

#define OUTPUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAPTURE V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define CAPTURE_COUNT 4

struct mapping {
	void *data;
	size_t size;
};

static void fail(const char *message)
{
	fprintf(stderr, "%s\n", message);
	exit(EXIT_FAILURE);
}

static void call(int fd, unsigned long op, void *arg, const char *name)
{
	int ret;

	do {
		ret = ioctl(fd, op, arg);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		perror(name);
		exit(EXIT_FAILURE);
	}
}

#define IOCTL(fd, op, arg) call(fd, op, arg, #op)

static int open_node(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	return fd;
}

static void read_full(FILE *f, void *dst, size_t size)
{
	if (fread(dst, 1, size, f) != size)
		fail("truncated fixture");
}

static void map_queue(int video, enum v4l2_buf_type type, unsigned int count,
		      struct mapping *map, bool requests)
{
	struct v4l2_requestbuffers req = {
		.count = count, .type = type, .memory = V4L2_MEMORY_MMAP,
	};
	unsigned int i;

	IOCTL(video, VIDIOC_REQBUFS, &req);
	if (req.count < count)
		fail("too few buffers");
	if (type == OUTPUT && !!(req.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS) != requests)
		fail("unexpected OUTPUT Media Request capability");
	for (i = 0; i < count; i++) {
		struct v4l2_plane plane = {};
		struct v4l2_buffer buf = {
			.type = type, .memory = V4L2_MEMORY_MMAP, .index = i,
			.length = 1, .m.planes = &plane,
		};

		IOCTL(video, VIDIOC_QUERYBUF, &buf);
		map[i].size = plane.length;
		map[i].data = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
				   MAP_SHARED, video, plane.m.mem_offset);
		if (map[i].data == MAP_FAILED)
			fail("mmap failed");
	}
}

static int request(int video, int media, FILE *fixture, unsigned int *coding)
{
	struct v4l2_ctrl_mpeg2_sequence seq;
	struct v4l2_ctrl_mpeg2_picture pic;
	struct v4l2_ctrl_mpeg2_quantisation quant;
	struct v4l2_ext_control ctl[] = {
		{ .id = V4L2_CID_STATELESS_MPEG2_SEQUENCE, .size = sizeof(seq), .ptr = &seq },
		{ .id = V4L2_CID_STATELESS_MPEG2_PICTURE, .size = sizeof(pic), .ptr = &pic },
		{ .id = V4L2_CID_STATELESS_MPEG2_QUANTISATION, .size = sizeof(quant), .ptr = &quant },
	};
	struct v4l2_ext_controls controls = {
		.which = V4L2_CTRL_WHICH_REQUEST_VAL, .count = 3, .controls = ctl,
	};
	int fd;

	read_full(fixture, &seq, sizeof(seq));
	read_full(fixture, &pic, sizeof(pic));
	read_full(fixture, &quant, sizeof(quant));
	*coding = pic.picture_coding_type;
	IOCTL(media, MEDIA_IOC_REQUEST_ALLOC, &fd);
	controls.request_fd = fd;
	IOCTL(video, VIDIOC_S_EXT_CTRLS, &controls);
	return fd;
}

static void wait_ready(int fd, short events)
{
	struct pollfd pfd = { .fd = fd, .events = events };
	int ret;

	do {
		ret = poll(&pfd, 1, 3000);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0 || !(pfd.revents & events))
		fail("decode poll timed out");
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		fail("clock_gettime failed");
	return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

struct cpu_ticks {
	unsigned long long total, idle;
};

static void sample_cpu(struct cpu_ticks cpu[4])
{
	char line[256];
	FILE *f = fopen("/proc/stat", "r");
	unsigned int core;

	if (!f || !fgets(line, sizeof(line), f))
		fail("cannot open CPU statistics");
	for (core = 0; core < 4; core++) {
		unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
		unsigned int no;

		if (!fgets(line, sizeof(line), f) ||
		    sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu",
			   &no, &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) != 9 ||
		    no != core)
			fail("invalid CPU statistics");
		cpu[core].total = user + nice + sys + idle + iowait + irq + softirq + steal;
		cpu[core].idle = idle + iowait;
	}
	fclose(f);
}

static void verify_eos(int video)
{
	struct v4l2_decoder_cmd stop = { .cmd = V4L2_DEC_CMD_STOP };
	struct v4l2_decoder_cmd start = { .cmd = V4L2_DEC_CMD_START };
	struct v4l2_plane plane = {};
	struct v4l2_buffer buf = {
		.type = CAPTURE, .memory = V4L2_MEMORY_MMAP,
		.length = 1, .m.planes = &plane,
	};
	struct v4l2_event event;

	IOCTL(video, VIDIOC_DECODER_CMD, &stop);
	IOCTL(video, VIDIOC_QBUF, &buf);
	wait_ready(video, POLLIN);
	IOCTL(video, VIDIOC_DQBUF, &buf);
	wait_ready(video, POLLPRI);
	IOCTL(video, VIDIOC_DQEVENT, &event);
	if (!(buf.flags & V4L2_BUF_FLAG_LAST) || plane.bytesused || event.type != V4L2_EVENT_EOS)
		fail("invalid MPEG-1 EOS");
	IOCTL(video, VIDIOC_DECODER_CMD, &start);
}

static void verify_output(int video)
{
	struct v4l2_fmtdesc fmt = { .type = OUTPUT };
	bool mpeg1 = false, mpeg2 = false;

	for (; ioctl(video, VIDIOC_ENUM_FMT, &fmt) == 0; fmt.index++) {
		if (!(fmt.flags & V4L2_FMT_FLAG_COMPRESSED))
			continue;
		mpeg1 |= fmt.pixelformat == V4L2_PIX_FMT_MPEG1;
		mpeg2 |= fmt.pixelformat == V4L2_PIX_FMT_MPEG2_SLICE;
	}
	if (errno != EINVAL || !mpeg1 || !mpeg2)
		fail("missing compressed MPEG OUTPUT format");
}

int main(int argc, char **argv)
{
	struct mapping output, capture[CAPTURE_COUNT];
	struct cpu_ticks before[4], after[4];
	unsigned int header[4], i, pixels, codec;
	enum v4l2_buf_type output_type = OUTPUT, capture_type = CAPTURE;
	struct v4l2_format fmt = { .type = OUTPUT };
	unsigned int raw_format = V4L2_PIX_FMT_NV21;
	int video, media = -1, dma = -1, old = -1, recent = -1;
	uint64_t decode_ns = 0, start, elapsed;
	FILE *fixture, *raw = NULL;

	if (argc > 1 && !strcmp(argv[1], "--tiled")) {
		raw_format = MV320_TILED_8;
		argv++;
		argc--;
	}
	if (argc < 4 || argc > 5)
		fail("usage: vdec-mpeg-test [--tiled] VIDEO MEDIA FIXTURE [output.nv21]");
	fixture = fopen(argv[3], "rb");
	if (!fixture)
		fail("cannot open fixture");
	read_full(fixture, header, sizeof(header));
	codec = header[0];
	if ((codec != 1 && codec != 2) || header[1] < 32 || header[1] > 1920 ||
	    header[2] < 32 || header[2] > 1080 || !header[3])
		fail("invalid fixture header");
	if (argc == 5) {
		if (raw_format != V4L2_PIX_FMT_NV21)
			fail("file output requires NV21 CAPTURE");
		raw = fopen(argv[4], "wb");
		if (!raw)
			fail("cannot open output file");
	}
	video = open_node(argv[1]);
	verify_output(video);
	if (codec == 2)
		media = open_node(argv[2]);
	fmt.fmt.pix_mp.pixelformat = codec == 2 ? V4L2_PIX_FMT_MPEG2_SLICE : V4L2_PIX_FMT_MPEG1;
	fmt.fmt.pix_mp.width = header[1];
	fmt.fmt.pix_mp.height = header[2];
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 0x200000;
	IOCTL(video, VIDIOC_S_FMT, &fmt);
	if (fmt.fmt.pix_mp.pixelformat !=
	    (codec == 2 ? V4L2_PIX_FMT_MPEG2_SLICE : V4L2_PIX_FMT_MPEG1))
		fail("unsupported OUTPUT format");
	if (codec == 1) {
		struct v4l2_event_subscription sub = { .type = V4L2_EVENT_EOS };

		IOCTL(video, VIDIOC_SUBSCRIBE_EVENT, &sub);
	}
	fmt.type = CAPTURE;
	fmt.fmt.pix_mp.pixelformat = raw_format;
	IOCTL(video, VIDIOC_S_FMT, &fmt);
	pixels = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	if (fmt.fmt.pix_mp.pixelformat != raw_format ||
	    fmt.fmt.pix_mp.width != header[1] || fmt.fmt.pix_mp.height != header[2] ||
	    (raw_format == V4L2_PIX_FMT_NV21 && pixels != header[1] * header[2] * 3 / 2))
		fail("unexpected CAPTURE layout");

	map_queue(video, OUTPUT, 1, &output, codec == 2);
	map_queue(video, CAPTURE, CAPTURE_COUNT, capture, false);
	if (raw_format == MV320_TILED_8) {
		struct v4l2_exportbuffer exported = { .type = CAPTURE, .flags = O_CLOEXEC };

		IOCTL(video, VIDIOC_EXPBUF, &exported);
		dma = exported.fd;
		puts("Tiled CAPTURE DMA-BUF exported");
	}
	IOCTL(video, VIDIOC_STREAMON, &output_type);
	IOCTL(video, VIDIOC_STREAMON, &capture_type);
	sample_cpu(before);
	start = monotonic_ns();
	for (i = 0; i < header[3]; i++) {
		unsigned int bytes, coding = 0;
		int fd = -1, dst = 0;
		uint64_t submitted;
		struct v4l2_plane out_plane = {}, cap_plane = {};
		struct v4l2_buffer src = {
			.type = OUTPUT, .memory = V4L2_MEMORY_MMAP,
			.length = 1, .m.planes = &out_plane,
			.timestamp = { .tv_usec = i + 1 },
		};
		struct v4l2_buffer cap = {
			.type = CAPTURE, .memory = V4L2_MEMORY_MMAP,
			.length = 1, .m.planes = &cap_plane,
		};

		read_full(fixture, &bytes, sizeof(bytes));
		if (!bytes || bytes > output.size)
			fail("payload outside OUTPUT buffer");
		if (codec == 2) {
			fd = request(video, media, fixture, &coding);
			src.flags = V4L2_BUF_FLAG_REQUEST_FD;
			src.request_fd = fd;
		}
		read_full(fixture, output.data, bytes);
		while (dst == old || dst == recent)
			dst++;
		cap.index = dst;
		out_plane.bytesused = bytes;
		IOCTL(video, VIDIOC_QBUF, &cap);
		submitted = monotonic_ns();
		IOCTL(video, VIDIOC_QBUF, &src);
		if (fd >= 0)
			IOCTL(fd, MEDIA_REQUEST_IOC_QUEUE, NULL);
		wait_ready(video, POLLIN);
		IOCTL(video, VIDIOC_DQBUF, &cap);
		decode_ns += monotonic_ns() - submitted;
		IOCTL(video, VIDIOC_DQBUF, &src);
		if (fd >= 0)
			close(fd);
		if ((src.flags | cap.flags) & V4L2_BUF_FLAG_ERROR ||
		    cap.index != (unsigned int)dst || cap_plane.bytesused != pixels) {
			fprintf(stderr, "decode failed at frame %u flags=%#x/%#x payload=%u\n",
				i, src.flags, cap.flags, cap_plane.bytesused);
			return EXIT_FAILURE;
		}
		if (codec == 2 && coding != V4L2_MPEG2_PIC_CODING_TYPE_B) {
			old = recent;
			recent = dst;
		}
		if (raw && fwrite(capture[dst].data, 1, pixels, raw) != pixels)
			fail("cannot write frame");
	}
	elapsed = monotonic_ns() - start;
	sample_cpu(after);
	if (codec == 1)
		verify_eos(video);
	IOCTL(video, VIDIOC_STREAMOFF, &output_type);
	IOCTL(video, VIDIOC_STREAMOFF, &capture_type);
	if (dma >= 0)
		close(dma);
	for (i = 0; i < CAPTURE_COUNT; i++)
		munmap(capture[i].data, capture[i].size);
	munmap(output.data, output.size);
	close(video);
	if (media >= 0)
		close(media);
	if (raw && fclose(raw))
		fail("cannot close output file");
	fclose(fixture);
	printf("MPEG%u decoded=%u fps=%.3f ms=%.3f elapsed_ms=%.3f\n", codec, header[3],
	       header[3] * 1e9 / elapsed, decode_ns / (1e6 * header[3]), elapsed / 1e6);
	for (i = 0; i < 4; i++) {
		unsigned long long total = after[i].total - before[i].total;
		unsigned long long idle = after[i].idle - before[i].idle;

		printf("CPU%u %.2f%%\n", i, total ? (total - idle) * 100.0 / total : 0);
	}
	return EXIT_SUCCESS;
}
