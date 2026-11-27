// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define BUFFERS 3
#define CORES 4
#define MAX_FRAMES 1024
#define MAX_JPEG (32U * 1024 * 1024)
#define STREAM_GUARD 128

struct frame {
	void *data;
	size_t size;
};

struct slot {
	void *data;
	size_t size;
	int dmabuf;
	struct timespec queued;
};

struct queue {
	enum v4l2_buf_type type;
	unsigned int count;
	struct slot slots[BUFFERS];
};

struct cpu_sample {
	unsigned long long total;
	unsigned long long idle;
};

static void fail(const char *message)
{
	fprintf(stderr, "%s\n", message);
	exit(EXIT_FAILURE);
}

static void fail_errno(const char *message)
{
	perror(message);
	exit(EXIT_FAILURE);
}

static void xioctl(int fd, unsigned long request, void *arg, const char *label)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0)
		fail_errno(label);
}

static unsigned int number(const char *arg, unsigned int min, unsigned int max)
{
	char *end;
	unsigned long val;

	errno = 0;
	val = strtoul(arg, &end, 10);
	if (errno || end == arg || *end || val < min || val > max)
		fail("Invalid numeric argument");
	return val;
}

static struct frame read_data(FILE *file, size_t size)
{
	struct frame frame = { .size = size };

	if (size < 4 || size > MAX_JPEG - STREAM_GUARD)
		fail("JPEG data size outside supported range");
	frame.data = malloc(frame.size);
	if (!frame.data)
		fail_errno("malloc");
	if (fread(frame.data, 1, frame.size, file) != frame.size)
		fail("Could not read JPEG data");
	return frame;
}

static struct frame read_frame(const char *path)
{
	struct frame frame;
	FILE *file = fopen(path, "rb");
	long length;

	if (!file)
		fail_errno(path);
	if (fseek(file, 0, SEEK_END))
		fail_errno(path);
	length = ftell(file);
	if (length < 0)
		fail_errno(path);
	rewind(file);
	frame = read_data(file, length);
	fclose(file);
	return frame;
}

static struct timespec now(void)
{
	struct timespec time;

	if (clock_gettime(CLOCK_MONOTONIC, &time))
		fail_errno("clock_gettime");
	return time;
}

static double elapsed_ms(struct timespec start, struct timespec end)
{
	return (end.tv_sec - start.tv_sec) * 1000.0 +
	       (end.tv_nsec - start.tv_nsec) / 1e6;
}

static void read_cpu(struct cpu_sample samples[CORES])
{
	char line[512];
	FILE *file = fopen("/proc/stat", "r");

	if (!file)
		fail_errno("/proc/stat");
	while (fgets(line, sizeof(line), file)) {
		unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
		unsigned int core;

		if (sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu",
			   &core, &user, &nice, &sys, &idle, &iowait,
			   &irq, &softirq, &steal) != 9 || core >= CORES)
			continue;
		samples[core].total = user + nice + sys + idle + iowait + irq + softirq + steal;
		samples[core].idle = idle + iowait;
	}
	fclose(file);
}

static struct v4l2_pix_format_mplane set_format(int fd, enum v4l2_buf_type type,
					       unsigned int width, unsigned int height,
					       uint32_t fourcc, size_t size)
{
	struct v4l2_format fmt = { .type = type };

	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.pixelformat = fourcc;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = size;
	xioctl(fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT");
	if (fmt.fmt.pix_mp.pixelformat != fourcc || fmt.fmt.pix_mp.num_planes != 1)
		fail("Driver did not accept requested format");
	return fmt.fmt.pix_mp;
}

static void setup_queue(int fd, struct queue *queue, unsigned int count, int export)
{
	struct v4l2_requestbuffers req = {
		.count = count,
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
	};
	unsigned int i;

	xioctl(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS");
	if (req.count < count)
		fail("Insufficient V4L2 buffers");
	queue->count = count;
	for (i = 0; i < count; i++) {
		struct v4l2_plane plane = { 0 };
		struct v4l2_buffer buf = {
			.index = i,
			.type = queue->type,
			.memory = V4L2_MEMORY_MMAP,
			.length = 1,
			.m.planes = &plane,
		};
		struct slot *slot = &queue->slots[i];

		xioctl(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");
		slot->size = plane.length;
		slot->data = mmap(NULL, slot->size, PROT_READ | PROT_WRITE,
				  MAP_SHARED, fd, plane.m.mem_offset);
		if (slot->data == MAP_FAILED)
			fail_errno("mmap");
		slot->dmabuf = -1;
		if (export) {
			struct v4l2_exportbuffer exp = {
				.type = queue->type,
				.index = i,
				.flags = O_CLOEXEC | O_RDONLY,
			};

			xioctl(fd, VIDIOC_EXPBUF, &exp, "VIDIOC_EXPBUF");
			slot->dmabuf = exp.fd;
		}
	}
}

static void queue_buffer(int fd, struct queue *queue, unsigned int index,
			 const struct frame *frame)
{
	struct slot *slot = &queue->slots[index];
	struct v4l2_plane plane = { .length = slot->size };
	struct v4l2_buffer buf = {
		.index = index,
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
		.length = 1,
		.m.planes = &plane,
	};

	if (frame) {
		if (frame->size > slot->size)
			fail("JPEG does not fit the OUTPUT buffer");
		memcpy(slot->data, frame->data, frame->size);
		plane.bytesused = frame->size;
		slot->queued = now();
		buf.timestamp.tv_sec = slot->queued.tv_sec;
		buf.timestamp.tv_usec = slot->queued.tv_nsec / 1000;
	}
	xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
}

static unsigned int dequeue(int fd, const struct queue *queue, unsigned int sequence,
			    int expect_error, size_t size)
{
	struct v4l2_plane plane = { 0 };
	struct v4l2_buffer buf = {
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
		.length = 1,
		.m.planes = &plane,
	};
	struct pollfd pollfd = {
		.fd = fd,
		.events = V4L2_TYPE_IS_OUTPUT(queue->type) ? POLLOUT : POLLIN,
	};
	int ret;

	for (;;) {
		ret = ioctl(fd, VIDIOC_DQBUF, &buf);
		if (!ret)
			break;
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN)
			fail_errno("VIDIOC_DQBUF");
		ret = poll(&pollfd, 1, 3000);
		if (ret < 0 && errno != EINTR)
			fail_errno("poll");
		if (!ret || (ret > 0 && !(pollfd.revents & pollfd.events)))
			fail("V4L2 queue stalled");
	}
	if (buf.index >= queue->count || buf.sequence != sequence ||
	    !!(buf.flags & V4L2_BUF_FLAG_ERROR) != expect_error ||
	    (size && (plane.bytesused != size || plane.data_offset))) {
		fprintf(stderr, "Unexpected DQBUF type=%u index=%u sequence=%u flags=%#x bytes=%u\n",
			buf.type, buf.index, buf.sequence, buf.flags, plane.bytesused);
		exit(EXIT_FAILURE);
	}
	return buf.index;
}

static void free_queue(int fd, struct queue *queue)
{
	struct v4l2_requestbuffers req = {
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
	};
	unsigned int i;

	for (i = 0; i < queue->count; i++) {
		struct slot *slot = &queue->slots[i];

		if (slot->dmabuf >= 0)
			close(slot->dmabuf);
		munmap(slot->data, slot->size);
	}
	xioctl(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS(0)");
}

static void write_yuv(FILE *file, const void *data, unsigned int width,
		      unsigned int height, unsigned int stride, unsigned int cap_height)
{
	const uint8_t *y = data;
	const uint8_t *uv = y + stride * cap_height;
	unsigned int uv_width = (width + 1) & ~1U;
	unsigned int row;
	for (row = 0; row < height; row++)
		if (fwrite(y + row * stride, 1, width, file) != width)
			fail_errno("write luma");
	for (row = 0; row < (height + 1) / 2; row++)
		if (fwrite(uv + row * stride, 1, uv_width, file) != uv_width)
			fail_errno("write chroma");
	if (fclose(file))
		fail_errno("close YUV stream");
}

static FILE *open_sink(const char *path, const char *host, unsigned int port)
{
	FILE *file;

	if (host) {
		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_port = htons(port),
		};
		int fd;

		if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
			fail("Invalid sink IPv4 address");
		fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
			fail_errno("connect YUV sink");
		file = fdopen(fd, "wb");
	} else {
		file = fopen(path, "wb");
	}
	if (!file)
		fail_errno("open YUV sink");
	return file;
}

int main(int argc, char **argv)
{
	struct queue output = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
	struct queue capture = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	struct cpu_sample before[CORES] = { 0 }, after[CORES] = { 0 };
	struct v4l2_selection selection = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.target = V4L2_SEL_TGT_COMPOSE,
	};
	struct v4l2_capability caps = { 0 };
	struct v4l2_pix_format_mplane out_fmt, cap_fmt;
	struct timespec start, end;
	struct frame *frames;
	uint32_t fourcc = V4L2_PIX_FMT_NV12;
	const char *yuv = NULL, *sink_host = NULL, *sequence = NULL;
	FILE *packed = NULL;
	unsigned int count = 1, i, width, height, queued, buffers, sink_port = 0;
	unsigned long long total_ticks = 0, idle_ticks = 0;
	double latency = 0, time_ms;
	size_t largest = 0;
	int fd, export = 0, expect_error = 0;

	if (argc < 5)
		fail("Usage: jpgd-test DEVICE WIDTH HEIGHT IMAGE [--yuv FILE | --send IP PORT] [--nv21] [--export]\n"
		     "       jpgd-test DEVICE WIDTH HEIGHT --sequence DIRECTORY COUNT [--nv21] [--export]\n"
		     "       jpgd-test DEVICE WIDTH HEIGHT --packed FILE COUNT [--nv21] [--export]\n"
		     "Sequence names are six digits. Packed JPEGs have a big-endian 32-bit size prefix.");
	width = number(argv[2], 64, 8192);
	height = number(argv[3], 64, 8192);
	i = 5;
	if (!strcmp(argv[4], "--sequence") || !strcmp(argv[4], "--packed")) {
		if (argc < 7)
			fail("Missing sequence source or frame count");
		if (!strcmp(argv[4], "--packed")) {
			packed = fopen(argv[5], "rb");
			if (!packed)
				fail_errno(argv[5]);
		} else {
			sequence = argv[5];
		}
		count = number(argv[6], 1, MAX_FRAMES);
		i = 7;
	}
	for (; i < (unsigned int)argc; i++) {
		if (!strcmp(argv[i], "--nv21"))
			fourcc = V4L2_PIX_FMT_NV21;
		else if (!strcmp(argv[i], "--export"))
			export = 1;
		else if (!strcmp(argv[i], "--expect-error") && !sequence && !packed)
			expect_error = 1;
		else if (!strcmp(argv[i], "--yuv") && i + 1 < (unsigned int)argc && count == 1)
			yuv = argv[++i];
		else if (!strcmp(argv[i], "--send") && i + 2 < (unsigned int)argc && count == 1) {
			sink_host = argv[++i];
			sink_port = number(argv[++i], 1, 65535);
		}
		else
			fail("Unknown option or output requested for multiple frames");
	}
	if ((expect_error && (yuv || sink_host)) || (yuv && sink_host))
		fail("Specify a single YUV sink for a successful frame");

	frames = calloc(count, sizeof(*frames));
	if (!frames)
		fail_errno("calloc");
	for (i = 0; i < count; i++) {
		char path[1024];

		if (sequence) {
			int len = snprintf(path, sizeof(path), "%s/%06u.jpg", sequence, i + 1);

			if (len < 0 || (size_t)len >= sizeof(path))
				fail("Sequence path is too long");
		}
		if (packed) {
			uint32_t size;

			if (fread(&size, sizeof(size), 1, packed) != 1)
				fail("Missing packed JPEG length");
			frames[i] = read_data(packed, ntohl(size));
		} else {
			frames[i] = read_frame(sequence ? path : argv[4]);
		}
		if (frames[i].size > largest)
			largest = frames[i].size;
	}
	if (packed) {
		if (fgetc(packed) != EOF || ferror(packed))
			fail("Unexpected trailing data in packed JPEG sequence");
		fclose(packed);
	}

	fd = open(argv[1], O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		fail_errno(argv[1]);
	xioctl(fd, VIDIOC_QUERYCAP, &caps, "VIDIOC_QUERYCAP");
	if (strcmp((const char *)caps.driver, "hi3798-jpgd") ||
	    !(caps.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE) ||
	    !(caps.device_caps & V4L2_CAP_STREAMING))
		fail("Device is not the Hi3798MV320 JPEG decoder");
	out_fmt = set_format(fd, output.type, width, height, V4L2_PIX_FMT_JPEG,
			     largest + STREAM_GUARD);
	cap_fmt = set_format(fd, capture.type, width, height, fourcc, 0);
	xioctl(fd, VIDIOC_G_SELECTION, &selection, "VIDIOC_G_SELECTION");
	if (out_fmt.width != width || out_fmt.height != height ||
	    out_fmt.plane_fmt[0].sizeimage < largest + STREAM_GUARD ||
	    selection.r.left || selection.r.top ||
	    selection.r.width != width || selection.r.height != height ||
	    cap_fmt.height < height || cap_fmt.plane_fmt[0].bytesperline < ((width + 1) & ~1U) ||
	    cap_fmt.plane_fmt[0].sizeimage != cap_fmt.plane_fmt[0].bytesperline * cap_fmt.height * 3 / 2)
		fail("Unexpected JPEG dimensions or capture layout");
	printf("device=%s driver=%s visible=%ux%u capture=%ux%u stride=%u format=%s buffers=%u\n",
	       argv[1], caps.driver, width, height, cap_fmt.width, cap_fmt.height,
	       cap_fmt.plane_fmt[0].bytesperline,
	       fourcc == V4L2_PIX_FMT_NV21 ? "NV21" : "NV12",
	       count < BUFFERS ? count : BUFFERS);

	buffers = count < BUFFERS ? count : BUFFERS;
	setup_queue(fd, &output, buffers, 0);
	setup_queue(fd, &capture, buffers, export);
	for (i = 0; i < buffers; i++)
		queue_buffer(fd, &capture, i, NULL);
	read_cpu(before);
	start = now();
	for (queued = 0; queued < buffers; queued++)
		queue_buffer(fd, &output, queued, &frames[queued]);
	xioctl(fd, VIDIOC_STREAMON, &capture.type, "VIDIOC_STREAMON(CAPTURE)");
	xioctl(fd, VIDIOC_STREAMON, &output.type, "VIDIOC_STREAMON(OUTPUT)");
	for (i = 0; i < count; i++) {
		unsigned int dst = dequeue(fd, &capture, i, expect_error,
					   expect_error ? 0 : cap_fmt.plane_fmt[0].sizeimage);
		unsigned int src = dequeue(fd, &output, i, expect_error, 0);

		latency += elapsed_ms(output.slots[src].queued, now());
		if (yuv || sink_host)
			write_yuv(open_sink(yuv, sink_host, sink_port),
				  capture.slots[dst].data, width, height,
				  cap_fmt.plane_fmt[0].bytesperline, cap_fmt.height);
		if (queued < count) {
			queue_buffer(fd, &capture, dst, NULL);
			queue_buffer(fd, &output, src, &frames[queued++]);
		}
	}
	end = now();
	read_cpu(after);
	xioctl(fd, VIDIOC_STREAMOFF, &output.type, "VIDIOC_STREAMOFF(OUTPUT)");
	xioctl(fd, VIDIOC_STREAMOFF, &capture.type, "VIDIOC_STREAMOFF(CAPTURE)");
	free_queue(fd, &capture);
	free_queue(fd, &output);
	close(fd);

	time_ms = elapsed_ms(start, end);
	printf("frames=%u rejected=%u elapsed_ms=%.3f fps=%.3f ms_per_frame=%.3f mean_queue_latency_ms=%.3f\n",
	       expect_error ? 0 : count, expect_error ? count : 0, time_ms, count * 1000.0 / time_ms,
	       time_ms / count, latency / count);
	for (i = 0; i < CORES; i++) {
		unsigned long long total = after[i].total - before[i].total;
		unsigned long long idle = after[i].idle - before[i].idle;

		if (total)
			printf("cpu%u_busy=%.2f%% ", i, 100.0 * (total - idle) / total);
		total_ticks += total;
		idle_ticks += idle;
	}
	if (total_ticks)
		printf("cpu4_average_busy=%.2f%%", 100.0 * (total_ticks - idle_ticks) / total_ticks);
	putchar('\n');

	for (i = 0; i < count; i++)
		free(frames[i].data);
	free(frames);
	return 0;
}
