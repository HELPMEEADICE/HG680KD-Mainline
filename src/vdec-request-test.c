// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the H.264 stateless UAPI with pre-parsed Annex B test slices. */

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
#include <unistd.h>

#define MAX_FRAMES 32
#define OUTPUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAPTURE V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

struct mapping {
	void *addr;
	size_t len;
};

struct picture {
	int poc;
	unsigned int frame_num;
	bool reference;
};

static void fail(const char *message)
{
	fprintf(stderr, "%s\n", message);
	exit(EXIT_FAILURE);
}

static void call(int fd, unsigned long cmd, void *arg, const char *name)
{
	int ret;

	do {
		ret = ioctl(fd, cmd, arg);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		perror(name);
		exit(EXIT_FAILURE);
	}
}

#define IOCTL(fd, cmd, arg) call(fd, cmd, arg, #cmd)

static int open_device(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	return fd;
}

static void map_buffers(int video, enum v4l2_buf_type type, unsigned int count,
			struct mapping *maps)
{
	struct v4l2_requestbuffers req = {
		.count = count, .type = type, .memory = V4L2_MEMORY_MMAP,
	};
	unsigned int i;

	IOCTL(video, VIDIOC_REQBUFS, &req);
	if (req.count < count)
		fail("driver returned too few buffers");
	if (type == OUTPUT && !(req.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS))
		fail("OUTPUT lacks Media Request capability");

	for (i = 0; i < count; i++) {
		struct v4l2_plane plane = {};
		struct v4l2_buffer buf = {
			.index = i, .type = type, .memory = V4L2_MEMORY_MMAP,
			.length = 1, .m.planes = &plane,
		};

		IOCTL(video, VIDIOC_QUERYBUF, &buf);
		maps[i].len = plane.length;
		maps[i].addr = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
				    MAP_SHARED, video, plane.m.mem_offset);
		if (maps[i].addr == MAP_FAILED) {
			perror("mmap");
			exit(EXIT_FAILURE);
		}
		if (type == CAPTURE)
			IOCTL(video, VIDIOC_QBUF, &buf);
	}
}

static void expect_request(int req)
{
	struct pollfd pfd = { .fd = req, .events = POLLPRI };
	int ret;

	do {
		ret = poll(&pfd, 1, 3000);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0 || !(pfd.revents & POLLPRI))
		fail("request did not complete");
}

static unsigned int dequeue(int video, enum v4l2_buf_type type,
			    unsigned int frame, unsigned int frame_size)
{
	struct v4l2_plane plane = {};
	struct v4l2_buffer buf = {
		.type = type, .memory = V4L2_MEMORY_MMAP, .length = 1,
		.m.planes = &plane,
	};

	IOCTL(video, VIDIOC_DQBUF, &buf);
	if (buf.flags & V4L2_BUF_FLAG_ERROR) {
		fprintf(stderr, "frame %u queue %u returned ERROR\n", frame, type);
		exit(EXIT_FAILURE);
	}
	if (type == CAPTURE && (plane.bytesused != frame_size ||
				buf.timestamp.tv_sec != frame + 1 ||
				buf.timestamp.tv_usec)) {
		fail("CAPTURE payload size or timestamp mismatch");
	}
	return buf.index;
}

static void set_reference(struct v4l2_ctrl_h264_decode_params *dec,
			  struct v4l2_h264_reference *ref, unsigned int slot,
			  const struct picture *pictures, unsigned int frame)
{
	struct v4l2_h264_dpb_entry *entry = &dec->dpb[slot];

	ref->index = slot;
	ref->fields = V4L2_H264_FRAME_REF;
	entry->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
	entry->reference_ts = (uint64_t)(frame + 1) * 1000000000;
	entry->fields = V4L2_H264_FRAME_REF;
	entry->frame_num = pictures[frame].frame_num;
	entry->pic_num = pictures[frame].frame_num;
	entry->top_field_order_cnt = pictures[frame].poc;
	entry->bottom_field_order_cnt = pictures[frame].poc;
}

static void set_references(struct v4l2_ctrl_h264_decode_params *dec,
			   struct v4l2_ctrl_h264_slice_params *slice,
			   const struct picture *pictures, unsigned int frame)
{
	int last = -1, past = -1, future = -1;
	unsigned int i;

	for (i = 0; i < frame; i++) {
		if (!pictures[i].reference)
			continue;
		last = i;
		if (pictures[i].poc < dec->top_field_order_cnt &&
		    (past < 0 || pictures[i].poc > pictures[past].poc))
			past = i;
		if (pictures[i].poc > dec->top_field_order_cnt &&
		    (future < 0 || pictures[i].poc < pictures[future].poc))
			future = i;
	}

	if (slice->slice_type == V4L2_H264_SLICE_TYPE_P && last >= 0) {
		dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
		set_reference(dec, &slice->ref_pic_list0[0], frame * 5 % V4L2_H264_NUM_DPB_ENTRIES,
			      pictures, last);
	} else if (slice->slice_type == V4L2_H264_SLICE_TYPE_B && past >= 0 && future >= 0) {
		dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
		set_reference(dec, &slice->ref_pic_list0[0], 0, pictures, past);
		set_reference(dec, &slice->ref_pic_list1[0], 15, pictures, future);
	} else if (!(dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC) ||
		   slice->slice_type != V4L2_H264_SLICE_TYPE_I) {
		fail("fixture has an unexpected slice or missing reference");
	}
}

int main(int argc, char **argv)
{
	enum v4l2_buf_type output = OUTPUT, capture = CAPTURE;
	struct v4l2_capability cap = {};
	struct v4l2_format fmt = { .type = OUTPUT };
	struct v4l2_fmtdesc nv21 = { .type = CAPTURE };
	struct v4l2_ext_control mode[] = {
		{ .id = V4L2_CID_STATELESS_H264_DECODE_MODE },
		{ .id = V4L2_CID_STATELESS_H264_START_CODE },
	};
	struct v4l2_ext_controls modes = {
		.which = V4L2_CTRL_WHICH_CUR_VAL,
		.count = sizeof(mode) / sizeof(mode[0]),
		.controls = mode,
	};
	struct v4l2_ctrl_h264_sps sps = {};
	struct v4l2_ctrl_h264_pps pps = {};
	struct mapping src, dst[MAX_FRAMES];
	unsigned int width, height, frames, profile, level, constraints, pps_flags, sps_flags;
	unsigned int log2_frame_num, poc_type, log2_poc, max_refs, frame_size, i;
	struct picture pictures[MAX_FRAMES];
	int qp, chroma, chroma2, video, media;
	FILE *manifest, *stream, *raw;
	bool test_flush = getenv("VDEC_TEST_FLUSH") != NULL;

	if (argc != 6)
		fail("usage: vdec-request /dev/videoN /dev/mediaN manifest slices.264 output.nv21");

	video = open_device(argv[1]);
	media = open_device(argv[2]);
	manifest = fopen(argv[3], "r");
	stream = fopen(argv[4], "rb");
	raw = fopen(argv[5], "wb");
	if (!manifest || !stream || !raw) {
		perror("fixture/output file");
		return EXIT_FAILURE;
	}

	if (fscanf(manifest, "%u%u%u%u%u%u%u%u%u%u%u%u%d%d%d",
		   &width, &height, &frames, &profile, &level, &constraints,
		   &log2_frame_num, &poc_type, &log2_poc, &max_refs,
		   &sps_flags, &pps_flags, &qp, &chroma, &chroma2) != 15 ||
	    frames < 1 || frames > MAX_FRAMES || !width || !height ||
	    width % 16 || height % 16)
		fail("invalid fixture header");

	sps = (struct v4l2_ctrl_h264_sps) {
		.profile_idc = profile, .constraint_set_flags = constraints,
		.level_idc = level, .chroma_format_idc = 1,
		.log2_max_frame_num_minus4 = log2_frame_num, .pic_order_cnt_type = poc_type,
		.log2_max_pic_order_cnt_lsb_minus4 = log2_poc, .max_num_ref_frames = max_refs,
		.pic_width_in_mbs_minus1 = width / 16 - 1,
		.pic_height_in_map_units_minus1 = height / 16 - 1, .flags = sps_flags,
	};
	pps = (struct v4l2_ctrl_h264_pps) {
		.pic_init_qp_minus26 = qp, .chroma_qp_index_offset = chroma,
		.second_chroma_qp_index_offset = chroma2, .flags = pps_flags,
	};

	IOCTL(video, VIDIOC_QUERYCAP, &cap);
	if (!(cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE))
		fail("expected a multiplanar M2M decoder");
	printf("%s on %s\n", cap.card, cap.bus_info);
	IOCTL(video, VIDIOC_G_EXT_CTRLS, &modes);
	if (mode[0].value != V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED ||
	    mode[1].value != V4L2_STATELESS_H264_START_CODE_ANNEX_B)
		fail("expected slice-based Annex B input");
	IOCTL(video, VIDIOC_ENUM_FMT, &nv21);
	if (nv21.pixelformat != V4L2_PIX_FMT_NV21)
		fail("expected NV21 CAPTURE format");

	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_SLICE;
	IOCTL(video, VIDIOC_S_FMT, &fmt);
	if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_H264_SLICE)
		fail("expected H264_SLICE OUTPUT format");
	fmt.type = CAPTURE;
	IOCTL(video, VIDIOC_G_FMT, &fmt);
	frame_size = width * height * 3 / 2;
	if (fmt.fmt.pix_mp.width != width || fmt.fmt.pix_mp.height != height ||
	    fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV21 ||
	    fmt.fmt.pix_mp.plane_fmt[0].bytesperline != width ||
	    fmt.fmt.pix_mp.plane_fmt[0].sizeimage != frame_size)
		fail("unexpected CAPTURE dimensions or pixel layout");

	map_buffers(video, OUTPUT, 1, &src);
	map_buffers(video, CAPTURE, frames, dst);
	IOCTL(video, VIDIOC_STREAMON, &output);
	IOCTL(video, VIDIOC_STREAMON, &capture);

	for (i = 0; i < frames;) {
		struct v4l2_ctrl_h264_slice_params slice = {};
		struct v4l2_ctrl_h264_decode_params dec = {};
		struct v4l2_ext_control controls[] = {
			{ .id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof(sps), .ptr = &sps },
			{ .id = V4L2_CID_STATELESS_H264_PPS, .size = sizeof(pps), .ptr = &pps },
			{ .id = V4L2_CID_STATELESS_H264_SLICE_PARAMS, .size = sizeof(slice), .ptr = &slice },
			{ .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS, .size = sizeof(dec), .ptr = &dec },
		};
		struct v4l2_ext_controls ext = {
			.which = V4L2_CTRL_WHICH_REQUEST_VAL,
			.count = sizeof(controls) / sizeof(controls[0]),
			.controls = controls,
		};
		struct v4l2_plane plane = {};
		struct v4l2_buffer qbuf = {
			.type = OUTPUT, .memory = V4L2_MEMORY_MMAP, .length = 1,
			.flags = V4L2_BUF_FLAG_REQUEST_FD, .m.planes = &plane,
			.timestamp.tv_sec = i + 1,
		};
		unsigned int bytes, bits, frame_num, type, idr, ref_idc, cabac_init, deblock;
		unsigned int idx, first_mb, hold, direct_spatial;
		int delta, poc, alpha, beta, req;

		if (fscanf(manifest, "%u%u%u%u%u%u%u%u%d%d%d%d%u%u%u",
			   &bytes, &bits, &frame_num, &type, &idr, &ref_idc, &cabac_init,
			   &deblock, &delta, &poc, &alpha, &beta, &first_mb, &hold,
			   &direct_spatial) != 15 ||
		    bytes > src.len || fread(src.addr, 1, bytes, stream) != bytes)
			fail("invalid fixture slice");

		slice.header_bit_size = bits;
		slice.first_mb_in_slice = first_mb;
		if (hold || (test_flush && i == frames - 1))
			qbuf.flags |= V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
		slice.slice_type = type;
		if (direct_spatial)
			slice.flags = V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
		slice.cabac_init_idc = cabac_init;
		slice.slice_qp_delta = delta;
		slice.disable_deblocking_filter_idc = deblock;
		slice.slice_alpha_c0_offset_div2 = alpha;
		slice.slice_beta_offset_div2 = beta;
		dec.nal_ref_idc = ref_idc;
		dec.frame_num = frame_num;
		dec.top_field_order_cnt = poc;
		dec.bottom_field_order_cnt = poc;
		dec.flags = idr ? V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC : 0;

		set_references(&dec, &slice, pictures, i);

		IOCTL(media, MEDIA_IOC_REQUEST_ALLOC, &req);
		ext.request_fd = req;
		if (first_mb) {
			ext.controls = &controls[2];
			ext.count = 1;
		} else if (i) {
			/* SPS is unchanged after the first picture. */
			ext.controls++;
			ext.count--;
		}
		IOCTL(video, VIDIOC_S_EXT_CTRLS, &ext);
		qbuf.request_fd = req;
		plane.bytesused = bytes;
		IOCTL(video, VIDIOC_QBUF, &qbuf);
		IOCTL(req, MEDIA_REQUEST_IOC_QUEUE, NULL);
		expect_request(req);
		dequeue(video, OUTPUT, i, 0);
		close(req);
		if (hold)
			continue;
		if (test_flush && i == frames - 1) {
			struct v4l2_decoder_cmd flush = { .cmd = V4L2_DEC_CMD_FLUSH };

			IOCTL(video, VIDIOC_DECODER_CMD, &flush);
		}
		idx = dequeue(video, CAPTURE, i, frame_size);
		if (idx >= frames || fwrite(dst[idx].addr, frame_size, 1, raw) != 1)
			fail("cannot write output frame");
		pictures[i] = (struct picture) { poc, frame_num, ref_idc != 0 };
		printf("frame %u: %ux%u NV21 %u bytes, CAPTURE index %u\n",
		       i, width, height, frame_size, idx);
		i++;
	}
	IOCTL(video, VIDIOC_STREAMOFF, &output);
	IOCTL(video, VIDIOC_STREAMOFF, &capture);
	for (i = 0; i < frames; i++)
		munmap(dst[i].addr, dst[i].len);
	munmap(src.addr, src.len);
	fclose(manifest);
	fclose(stream);
	close(video);
	close(media);
	return fclose(raw) ? EXIT_FAILURE : EXIT_SUCCESS;
}
