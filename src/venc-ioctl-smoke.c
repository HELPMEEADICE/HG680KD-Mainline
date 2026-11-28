// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

static void cmd(int fd, unsigned long request, void *arg, const char *name)
{
	if (ioctl(fd, request, arg) < 0) {
		perror(name);
		exit(1);
	}
}

static void require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "%s\n", message);
		exit(1);
	}
}

static void check_control(int fd, unsigned int id, int expected)
{
	struct v4l2_control control = { .id = id };

	cmd(fd, VIDIOC_G_CTRL, &control, "G_CTRL");
	require(control.value == expected, "unexpected control value");
	printf("control %#x = %d\n", id, expected);
}

static void drain_empty(int fd, int queue_before_stop)
{
	struct v4l2_plane plane = {};
	struct v4l2_buffer buffer = {
		.type = CAP, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &plane,
	};
	struct v4l2_encoder_cmd stop = { .cmd = V4L2_ENC_CMD_STOP };
	struct v4l2_event event = {};

	cmd(fd, VIDIOC_QUERYBUF, &buffer, "QUERYBUF CAPTURE");
	if (queue_before_stop)
		cmd(fd, VIDIOC_QBUF, &buffer, "QBUF before STOP");
	cmd(fd, VIDIOC_ENCODER_CMD, &stop, "STOP");
	if (!queue_before_stop) {
		require(ioctl(fd, VIDIOC_DQEVENT, &event) == -1 && errno == ENOENT,
			"EOS queued before CAPTURE buffer");
		cmd(fd, VIDIOC_QBUF, &buffer, "QBUF after STOP");
	}
	cmd(fd, VIDIOC_DQEVENT, &event, "DQEVENT");
	require(event.type == V4L2_EVENT_EOS, "expected EOS");
	cmd(fd, VIDIOC_DQBUF, &buffer, "DQBUF");
	require((buffer.flags & V4L2_BUF_FLAG_LAST) && !plane.bytesused, "expected empty LAST");
	require(ioctl(fd, VIDIOC_DQBUF, &buffer) == -1 && errno == EPIPE, "expected EPIPE after LAST");
	cmd(fd, VIDIOC_ENCODER_CMD, &stop, "repeated STOP");
	require(ioctl(fd, VIDIOC_DQEVENT, &event) == -1 && errno == ENOENT, "duplicate EOS");
	printf("STOP with CAPTURE queued %s STOP: EOS, empty LAST, EPIPE exactly once\n",
	       queue_before_stop ? "before" : "after");
}

int main(int argc, char **argv)
{
	int fd = open("/dev/video2", O_RDWR | O_NONBLOCK);
	struct v4l2_streamparm parm = { .type = OUT };
	struct v4l2_queryctrl bit = { .id = V4L2_CID_MPEG_VIDEO_BITRATE };
	struct v4l2_control set_bit = { .id = V4L2_CID_MPEG_VIDEO_BITRATE, .value = 200000 };
	struct v4l2_event_subscription sub = { .type = V4L2_EVENT_EOS };
	struct v4l2_requestbuffers req = { .count = 1, .type = OUT, .memory = V4L2_MEMORY_MMAP };
	struct v4l2_encoder_cmd start = { .cmd = V4L2_ENC_CMD_START };
	unsigned int out = OUT, cap = CAP;

	require(fd >= 0, "cannot open /dev/video2");
	if (argc == 2 && !strcmp(argv[1], "--log-status")) {
		cmd(fd, VIDIOC_LOG_STATUS, NULL, "LOG_STATUS");
		close(fd);
		return 0;
	}
	require(argc == 1, "usage: venc-ioctl-smoke [--log-status]");
	cmd(fd, VIDIOC_G_PARM, &parm, "G_PARM OUTPUT");
	require(parm.parm.output.capability & V4L2_CAP_TIMEPERFRAME, "missing frame interval capability");
	require(parm.parm.output.timeperframe.numerator == 1 && parm.parm.output.timeperframe.denominator == 30,
		"unexpected default frame interval");
	parm.parm.output.timeperframe = (struct v4l2_fract){ 1001, 30000 };
	cmd(fd, VIDIOC_S_PARM, &parm, "S_PARM OUTPUT");
	parm.type = CAP;
	cmd(fd, VIDIOC_G_PARM, &parm, "G_PARM CAPTURE");
	require(parm.parm.capture.timeperframe.numerator == 1001 &&
		parm.parm.capture.timeperframe.denominator == 30000, "CAPTURE frame interval mismatch");
	puts("G_PARM OUTPUT default 1/30; S_PARM OUTPUT and G_PARM CAPTURE 1001/30000");
	check_control(fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE);
	check_control(fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_2);
	check_control(fd, V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
	check_control(fd, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0);
	cmd(fd, VIDIOC_QUERYCTRL, &bit, "QUERYCTRL BITRATE");
	require(bit.flags & V4L2_CTRL_FLAG_INACTIVE, "BITRATE unexpectedly active");
	cmd(fd, VIDIOC_S_CTRL, &set_bit, "S_CTRL BITRATE");
	require(!set_bit.value, "BITRATE request not normalized to zero");
	puts("BITRATE inactive; 200000 normalized to zero");
	cmd(fd, VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE_EVENT EOS");
	cmd(fd, VIDIOC_REQBUFS, &req, "REQBUFS OUTPUT");
	req.count = 1;
	req.type = CAP;
	cmd(fd, VIDIOC_REQBUFS, &req, "REQBUFS CAPTURE");
	cmd(fd, VIDIOC_STREAMON, &out, "STREAMON OUTPUT");
	cmd(fd, VIDIOC_STREAMON, &cap, "STREAMON CAPTURE");
	drain_empty(fd, 1);
	cmd(fd, VIDIOC_ENCODER_CMD, &start, "START");
	drain_empty(fd, 0);
	cmd(fd, VIDIOC_STREAMOFF, &cap, "STREAMOFF CAPTURE");
	cmd(fd, VIDIOC_STREAMOFF, &out, "STREAMOFF OUTPUT");
	close(fd);
	return 0;
}
