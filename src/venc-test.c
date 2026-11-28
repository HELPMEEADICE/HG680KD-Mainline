// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the HG680KD V4L2 H.264 encoder with packed NV12 or NV21 frames. */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OUTPUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAPTURE V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define CPUS 4

struct buffer {
	enum v4l2_buf_type type;
	unsigned char *addr;
	size_t size;
};

struct counters {
	unsigned long long total[CPUS], idle[CPUS], interrupts;
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

#define CALL(fd, op, arg) call(fd, op, arg, #op)

static unsigned int number(const char *text)
{
	char *tail;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &tail, 10);
	if (*text < '0' || *text > '9' || *tail || errno || value > UINT_MAX)
		fail("invalid numeric argument");
	return value;
}

static uint64_t now_ns(void)
{
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t))
		fail("clock_gettime failed");
	return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void read_counters(struct counters *c)
{
	char line[512];
	FILE *file = fopen("/proc/stat", "r");
	unsigned int index = 0;

	if (!file)
		fail("cannot open /proc/stat");
	while (fgets(line, sizeof(line), file)) {
		unsigned long long t[8];
		unsigned int cpu, i;

		if (strncmp(line, "cpu", 3) || line[3] < '0' || line[3] > '9' ||
		    sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu",
			   &cpu, &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7]) != 9)
			continue;
		if (cpu >= CPUS)
			continue;
		c->total[cpu] = 0;
		for (i = 0; i < 8; i++)
			c->total[cpu] += t[i];
		c->idle[cpu] = t[3] + t[4];
		index++;
	}
	fclose(file);
	if (index != CPUS)
		fail("missing board CPU counters");

	file = fopen("/proc/interrupts", "r");
	if (!file)
		fail("cannot open /proc/interrupts");
	while (fgets(line, sizeof(line), file)) {
		unsigned int irq, hwirq;
		unsigned long long t[CPUS];

		if (sscanf(line, " %u: %llu %llu %llu %llu GICv3 %u",
			   &irq, &t[0], &t[1], &t[2], &t[3], &hwirq) == 6 && hwirq == 133) {
			c->interrupts = t[0] + t[1] + t[2] + t[3];
			fclose(file);
			return;
		}
	}
	fail("missing VENC GIC INTID 133 counter");
}

static void map_buffer(int fd, struct buffer *mem, enum v4l2_buf_type type)
{
	struct v4l2_requestbuffers req = { .count = 1, .type = type, .memory = V4L2_MEMORY_MMAP };
	struct v4l2_plane plane = {};
	struct v4l2_buffer buf = {
		.type = type, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &plane,
	};

	CALL(fd, VIDIOC_REQBUFS, &req);
	if (!req.count)
		fail("driver returned no MMAP buffers");
	CALL(fd, VIDIOC_QUERYBUF, &buf);
	mem->type = type;
	mem->size = plane.length;
	mem->addr = mmap(NULL, mem->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
	if (mem->addr == MAP_FAILED)
		fail("MMAP failed");
}

static void queue(int fd, const struct buffer *mem, size_t len, unsigned int frame)
{
	struct v4l2_plane plane = { .length = mem->size, .bytesused = len };
	struct v4l2_buffer buf = {
		.type = mem->type, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &plane,
		.field = V4L2_FIELD_NONE, .timestamp.tv_usec = (frame % 1000) * 1000,
		.timestamp.tv_sec = frame / 1000 + 1,
	};

	CALL(fd, VIDIOC_QBUF, &buf);
}

static size_t dequeue(int fd, enum v4l2_buf_type type, unsigned int frame,
		      unsigned int expected)
{
	struct v4l2_plane plane = {};
	struct v4l2_buffer buf = {
		.type = type, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &plane,
	};

	if (type == CAPTURE) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		if (poll(&pfd, 1, 3000) <= 0)
			fail("timed out waiting for CAPTURE buffer");
	}
	CALL(fd, VIDIOC_DQBUF, &buf);
	if ((buf.flags & V4L2_BUF_FLAG_ERROR) || buf.index || plane.data_offset)
		fail("driver returned an invalid or errored buffer");
	if (type == CAPTURE && expected == V4L2_BUF_FLAG_LAST &&
	    (!(buf.flags & V4L2_BUF_FLAG_LAST) || plane.bytesused))
		fail("expected an empty LAST buffer at end of the stream");
	if (type == CAPTURE && expected != V4L2_BUF_FLAG_LAST &&
	    ((buf.flags & (V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
			   V4L2_BUF_FLAG_BFRAME | V4L2_BUF_FLAG_LAST)) != expected ||
	     buf.sequence != frame || buf.timestamp.tv_sec != frame / 1000 + 1 ||
	     buf.timestamp.tv_usec != (frame % 1000) * 1000 || !plane.bytesused))
		fail("CAPTURE frame type, sequence, timestamp or payload mismatch");
	return plane.bytesused;
}

static void set_control(int fd, uint32_t id, unsigned int value)
{
	struct v4l2_control ctrl = { .id = id, .value = value };

	CALL(fd, VIDIOC_S_CTRL, &ctrl);
	if (id != V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME) {
		CALL(fd, VIDIOC_G_CTRL, &ctrl);
		if (ctrl.value != (int)value)
			fail("V4L2 encoder control did not retain its value");
	}
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "gop", required_argument, NULL, 'g' },
		{ "i-qp", required_argument, NULL, 'i' },
		{ "p-qp", required_argument, NULL, 'p' },
		{ "input-fps", required_argument, NULL, 'r' },
		{ "force-key-at", required_argument, NULL, 'k' },
		{}
	};
	struct v4l2_capability cap = {};
	struct v4l2_format format = { .type = CAPTURE };
	struct v4l2_encoder_cmd stop = { .cmd = V4L2_ENC_CMD_STOP };
	struct v4l2_encoder_cmd start = { .cmd = V4L2_ENC_CMD_START };
	struct buffer src, dst;
	struct counters before, after;
	unsigned int w, h, frames, stride, payload, fourcc, i, row;
	unsigned int gop = 0, i_qp = 28, p_qp = UINT_MAX, input_fps = 30;
	unsigned int force_key_at = UINT_MAX, gop_position = 0, i_frames = 0;
	unsigned long long i_bytes = 0, p_bytes = 0;
	uint64_t begin, elapsed, latency = 0, worst = 0;
	FILE *input, *output;
	double kbps;
	int fd, option;

	while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
		switch (option) {
		case 'g':
			gop = number(optarg);
			if (!gop)
				fail("GOP must be positive");
			break;
		case 'i':
			i_qp = number(optarg);
			break;
		case 'p':
			p_qp = number(optarg);
			if (p_qp > 51)
				fail("P frame QP exceeds 51");
			break;
		case 'r':
			input_fps = number(optarg);
			break;
		case 'k':
			force_key_at = number(optarg);
			break;
		default:
			return EXIT_FAILURE;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 7 && argc != 8)
		fail("usage: venc-test [--gop N] [--i-qp QP] [--p-qp QP] [--input-fps FPS] "
		     "[--force-key-at FRAME] /dev/video2 WIDTH HEIGHT NV12|NV21 [legacy QP] "
		     "FRAMES input.raw output.h264");
	w = number(argv[1]);
	h = number(argv[2]);
	if (!strcmp(argv[3], "NV12"))
		fourcc = V4L2_PIX_FMT_NV12;
	else if (!strcmp(argv[3], "NV21"))
		fourcc = V4L2_PIX_FMT_NV21;
	else
		fail("input format must be NV12 or NV21");
	if (argc == 8) {
		i_qp = number(argv[4]);
		if (!gop)
			gop = 1;
	}
	if (!gop)
		gop = 30;
	if (p_qp == UINT_MAX)
		p_qp = i_qp;
	frames = number(argv[argc - 3]);
	if (w < 256 || w > 1920 || h < 128 || h > 1088 || (w | h) & 1 ||
	    i_qp > 51 || gop > 120 || !input_fps || !frames || frames > 100000 ||
	    (force_key_at != UINT_MAX && force_key_at >= frames))
		fail("dimensions, GOP, QP, source rate or frame index are out of range");
	input = fopen(argv[argc - 2], "rb");
	if (!input)
		fail("cannot open input file");
	output = fopen(argv[argc - 1], "wb");
	if (!output)
		fail("cannot open output file");
	fd = open(argv[0], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(argv[0]);
		return EXIT_FAILURE;
	}
	CALL(fd, VIDIOC_QUERYCAP, &cap);
	if (strcmp((const char *)cap.driver, "hi3798-venc") ||
	    !(cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE) || !(cap.device_caps & V4L2_CAP_STREAMING))
		fail("unexpected VENC capabilities");
	format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
	CALL(fd, VIDIOC_S_FMT, &format);
	format.type = OUTPUT;
	format.fmt.pix_mp.width = w;
	format.fmt.pix_mp.height = h;
	format.fmt.pix_mp.pixelformat = fourcc;
	CALL(fd, VIDIOC_S_FMT, &format);
	stride = format.fmt.pix_mp.plane_fmt[0].bytesperline;
	payload = format.fmt.pix_mp.plane_fmt[0].sizeimage;
	if (format.fmt.pix_mp.width != w || format.fmt.pix_mp.height != h ||
	    format.fmt.pix_mp.pixelformat != fourcc || format.fmt.pix_mp.num_planes != 1 ||
	    stride != ((w + 63) & ~63U) || payload != stride * h * 3 / 2)
		fail("unexpected OUTPUT layout");
	format.type = CAPTURE;
	CALL(fd, VIDIOC_G_FMT, &format);
	if (format.fmt.pix_mp.width != w || format.fmt.pix_mp.height != h ||
	    format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_H264 ||
	    format.fmt.pix_mp.num_planes != 1)
		fail("unexpected CAPTURE layout");
	set_control(fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop);
	set_control(fd, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, i_qp);
	if (gop > 1)
		set_control(fd, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, p_qp);
	map_buffer(fd, &src, OUTPUT);
	map_buffer(fd, &dst, CAPTURE);
	CALL(fd, VIDIOC_STREAMON, &src.type);
	CALL(fd, VIDIOC_STREAMON, &dst.type);

	read_counters(&before);
	begin = now_ns();
	for (i = 0; i < frames; i++) {
		uint64_t submit, wait;
		size_t used;
		int key;

		if (stride == w) {
			if (fread(src.addr, 1, payload, input) != payload)
				fail("short raw input frame");
		} else {
			for (row = 0; row < h * 3 / 2; row++)
				if (fread(src.addr + row * stride, 1, w, input) != w)
					fail("short raw input row");
		}
		if (i == force_key_at) {
			set_control(fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0);
			gop_position = 0;
		}
		key = !gop_position;
		gop_position = (gop_position + 1) % gop;
		queue(fd, &dst, 0, 0);
		submit = now_ns();
		queue(fd, &src, payload, i);
		used = dequeue(fd, CAPTURE, i, key ? V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME);
		dequeue(fd, OUTPUT, i, 0);
		wait = now_ns() - submit;
		latency += wait;
		if (wait > worst)
			worst = wait;
		if (used > dst.size || fwrite(dst.addr, 1, used, output) != used)
			fail("invalid bytesused or failed to write H.264");
		if (key) {
			i_bytes += used;
			i_frames++;
		} else {
			p_bytes += used;
		}
	}
	elapsed = now_ns() - begin;
	read_counters(&after);

	CALL(fd, VIDIOC_ENCODER_CMD, &stop);
	queue(fd, &dst, 0, 0);
	dequeue(fd, CAPTURE, frames, V4L2_BUF_FLAG_LAST);
	CALL(fd, VIDIOC_ENCODER_CMD, &start);
	CALL(fd, VIDIOC_STREAMOFF, &src.type);
	CALL(fd, VIDIOC_STREAMOFF, &dst.type);
	CALL(fd, VIDIOC_LOG_STATUS, NULL);
	munmap(src.addr, src.size);
	munmap(dst.addr, dst.size);
	close(fd);
	fclose(input);
	if (fclose(output))
		fail("failed to close H.264 output");

	kbps = (i_bytes + p_bytes) * (8.0 * input_fps / (1000.0 * frames));
	printf("VENC %ux%u %s GOP=%u I_QP=%u P_QP=%u frames=%u bytes=%llu fps=%.3f avg_ms=%.3f max_ms=%.3f irq=%llu\n",
	       w, h, argv[3], gop, i_qp, p_qp, frames, i_bytes + p_bytes, frames * 1e9 / elapsed,
	       latency / (frames * 1e6), worst / 1e6, after.interrupts - before.interrupts);
	printf("RATE input_fps=%u kbps=%.3f Mbps=%.6f i_frames=%u p_frames=%u i_bytes_avg=%.2f p_bytes_avg=%.2f\n",
	       input_fps, kbps, kbps / 1000, i_frames, frames - i_frames,
	       (double)i_bytes / i_frames, frames == i_frames ? 0.0 : (double)p_bytes / (frames - i_frames));
	for (i = 0; i < CPUS; i++) {
		unsigned long long total = after.total[i] - before.total[i];
		unsigned long long idle = after.idle[i] - before.idle[i];

		printf("CPU%u %.2f%%\n", i, total ? 100.0 * (total - idle) / total : 0.0);
	}
	return after.interrupts - before.interrupts == frames ? EXIT_SUCCESS : EXIT_FAILURE;
}
