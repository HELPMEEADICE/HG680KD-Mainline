// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the stateless UAPI with pre-parsed Annex B test slices. */

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

static unsigned int setup_decoder(int video, uint32_t fourcc, unsigned int width,
				  unsigned int height, unsigned int frames,
				  struct mapping *src, struct mapping *dst)
{
	enum v4l2_buf_type output = OUTPUT, capture = CAPTURE;
	unsigned int frame_size = width * height * 3 / 2;
	struct v4l2_format fmt = { .type = OUTPUT };

	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.pixelformat = fourcc;
	IOCTL(video, VIDIOC_S_FMT, &fmt);
	if (fmt.fmt.pix_mp.pixelformat != fourcc)
		fail("unexpected OUTPUT format");
	fmt.type = CAPTURE;
	IOCTL(video, VIDIOC_G_FMT, &fmt);
	if (fmt.fmt.pix_mp.width != width || fmt.fmt.pix_mp.height != height ||
	    fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV21 ||
	    fmt.fmt.pix_mp.plane_fmt[0].bytesperline != width ||
	    fmt.fmt.pix_mp.plane_fmt[0].sizeimage != frame_size)
		fail("unexpected CAPTURE dimensions or pixel layout");
	map_buffers(video, OUTPUT, 1, src);
	map_buffers(video, CAPTURE, frames, dst);
	IOCTL(video, VIDIOC_STREAMON, &output);
	IOCTL(video, VIDIOC_STREAMON, &capture);
	return frame_size;
}

static void queue_slice(int video, int media, struct v4l2_ext_controls *ext,
			unsigned int frame, unsigned int bytes, bool hold)
{
	struct v4l2_plane plane = { .bytesused = bytes };
	struct v4l2_buffer qbuf = {
		.type = OUTPUT, .memory = V4L2_MEMORY_MMAP, .length = 1,
		.flags = V4L2_BUF_FLAG_REQUEST_FD, .m.planes = &plane,
		.timestamp.tv_sec = frame + 1,
	};
	int req;

	if (hold)
		qbuf.flags |= V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
	IOCTL(media, MEDIA_IOC_REQUEST_ALLOC, &req);
	ext->request_fd = req;
	IOCTL(video, VIDIOC_S_EXT_CTRLS, ext);
	qbuf.request_fd = req;
	IOCTL(video, VIDIOC_QBUF, &qbuf);
	IOCTL(req, MEDIA_REQUEST_IOC_QUEUE, NULL);
	expect_request(req);
	dequeue(video, OUTPUT, frame, 0);
	close(req);
}

static void read_hevc_weights(FILE *manifest, unsigned int count,
			      int8_t *luma_weight, int8_t *luma_offset,
			      int8_t (*chroma_weight)[2], int8_t (*chroma_offset)[2])
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (fscanf(manifest, "%hhd%hhd%hhd%hhd%hhd%hhd", &luma_weight[i], &luma_offset[i],
			   &chroma_weight[i][0], &chroma_offset[i][0],
			   &chroma_weight[i][1], &chroma_offset[i][1]) != 6)
			fail("invalid HEVC prediction weights");
	}
}

static int run_hevc(int video, int media, FILE *manifest, FILE *stream, FILE *raw)
{
	enum v4l2_buf_type output = OUTPUT, capture = CAPTURE;
	struct v4l2_ctrl_hevc_sps sps = {};
	struct v4l2_ctrl_hevc_pps pps = {};
	struct v4l2_ctrl_hevc_scaling_matrix scaling = {};
	struct v4l2_ext_control mode[] = {
		{ .id = V4L2_CID_STATELESS_HEVC_DECODE_MODE },
		{ .id = V4L2_CID_STATELESS_HEVC_START_CODE },
	};
	struct v4l2_ext_controls modes = {
		.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 2, .controls = mode,
	};
	struct v4l2_ext_control sps_control = {
		.id = V4L2_CID_STATELESS_HEVC_SPS, .size = sizeof(sps), .ptr = &sps,
	};
	struct v4l2_ext_controls sequence = {
		.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &sps_control,
	};
	struct v4l2_fmtdesc format = { .index = 1, .type = OUTPUT };
	struct mapping src, dst[MAX_FRAMES];
	unsigned int width, height, frames, frame_size, i;
	unsigned long long sps_flags, pps_flags;

	if (fscanf(manifest, "%u%u%u", &width, &height, &frames) != 3 ||
	    !frames || frames > MAX_FRAMES || !width || !height ||
	    width % 8 || height % 8 || width > 1920 || height > 1088)
		fail("invalid HEVC fixture header");
	if (fscanf(manifest, "%hhu%hhu%hhu%hhu%hhu%hhu%hhu%hhu%llu",
		   &sps.log2_min_luma_coding_block_size_minus3,
		   &sps.log2_diff_max_min_luma_coding_block_size,
		   &sps.log2_min_luma_transform_block_size_minus2,
		   &sps.log2_diff_max_min_luma_transform_block_size,
		   &sps.max_transform_hierarchy_depth_inter,
		   &sps.max_transform_hierarchy_depth_intra,
		   &sps.log2_max_pic_order_cnt_lsb_minus4,
		   &sps.num_short_term_ref_pic_sets, &sps_flags) != 9)
		fail("invalid HEVC SPS");
	if (fscanf(manifest, "%hhd%hhd%hhd%hhu%hhu%llu",
		   &pps.init_qp_minus26, &pps.pps_cb_qp_offset, &pps.pps_cr_qp_offset,
		   &pps.diff_cu_qp_delta_depth, &pps.log2_parallel_merge_level_minus2,
		   &pps_flags) != 6)
		fail("invalid HEVC PPS");
	sps.pic_width_in_luma_samples = width;
	sps.pic_height_in_luma_samples = height;
	sps.chroma_format_idc = 1;
	sps.flags = sps_flags;
	pps.flags = pps_flags;
	if (pps.flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) {
		if (fscanf(manifest, "%hhu%hhu", &pps.num_tile_columns_minus1,
			   &pps.num_tile_rows_minus1) != 2 ||
		    pps.num_tile_columns_minus1 >= sizeof(pps.column_width_minus1) ||
		    pps.num_tile_rows_minus1 >= sizeof(pps.row_height_minus1))
			fail("invalid HEVC tile counts");
		if (!(pps.flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING)) {
			for (i = 0; i < pps.num_tile_columns_minus1; i++)
				if (fscanf(manifest, "%hhu", &pps.column_width_minus1[i]) != 1)
					fail("invalid HEVC tile column width");
			for (i = 0; i < pps.num_tile_rows_minus1; i++)
				if (fscanf(manifest, "%hhu", &pps.row_height_minus1[i]) != 1)
					fail("invalid HEVC tile row height");
		}
	}
	if (sps.flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED)
		for (i = 0; i < sizeof(scaling); i++)
			if (fscanf(manifest, "%hhu", (uint8_t *)&scaling + i) != 1)
				fail("invalid HEVC scaling matrix");

	IOCTL(video, VIDIOC_G_EXT_CTRLS, &modes);
	if (mode[0].value != V4L2_STATELESS_HEVC_DECODE_MODE_SLICE_BASED ||
	    mode[1].value != V4L2_STATELESS_HEVC_START_CODE_ANNEX_B)
		fail("expected slice-based Annex B HEVC input");
	IOCTL(video, VIDIOC_ENUM_FMT, &format);
	if (format.pixelformat != V4L2_PIX_FMT_HEVC_SLICE)
		fail("missing HEVC OUTPUT format");
	IOCTL(video, VIDIOC_S_EXT_CTRLS, &sequence);
	frame_size = setup_decoder(video, V4L2_PIX_FMT_HEVC_SLICE, width, height, frames, &src, dst);

	for (i = 0; i < frames;) {
		struct v4l2_ctrl_hevc_slice_params slice = {};
		struct v4l2_ctrl_hevc_decode_params dec = {};
		struct v4l2_ext_control controls[] = {
			{ .id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS, .size = sizeof(slice), .ptr = &slice },
			{ .id = V4L2_CID_STATELESS_HEVC_PPS, .size = sizeof(pps), .ptr = &pps },
			{ .id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS, .size = sizeof(dec), .ptr = &dec },
			{ .id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX, .size = sizeof(scaling), .ptr = &scaling },
		};
		struct v4l2_ext_controls ext = {
			.which = V4L2_CTRL_WHICH_REQUEST_VAL, .count = 3, .controls = controls,
		};
		unsigned int bytes, index, j, hold, l0, l1;
		unsigned long long flags;

		if (fscanf(manifest, "%u%u%hhu%hhu%hhu%d%hhd%hhd%hhd%hhd%hhd%hhu%hhu%u%u%llu%u",
			   &bytes, &slice.data_byte_offset, &slice.nal_unit_type,
			   &slice.nuh_temporal_id_plus1, &slice.slice_type, &slice.slice_pic_order_cnt,
			   &slice.slice_qp_delta, &slice.slice_cb_qp_offset, &slice.slice_cr_qp_offset,
			   &slice.slice_beta_offset_div2, &slice.slice_tc_offset_div2,
			   &slice.five_minus_max_num_merge_cand, &slice.collocated_ref_idx,
			   &slice.slice_segment_addr, &hold, &flags, &slice.num_entry_point_offsets) != 17 ||
		    bytes > src.len || fread(src.addr, 1, bytes, stream) != bytes)
			fail("invalid HEVC slice");
		slice.bit_size = bytes * 8;
		slice.flags = flags;
		dec.pic_order_cnt_val = slice.slice_pic_order_cnt;
		if (slice.nal_unit_type >= 16 && slice.nal_unit_type <= 23)
			dec.flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC;
		if (slice.nal_unit_type == 19 || slice.nal_unit_type == 20)
			dec.flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC;
		if (fscanf(manifest, "%hhu%hhu%hhu%u%u", &dec.num_active_dpb_entries,
			   &dec.num_poc_st_curr_before, &dec.num_poc_st_curr_after, &l0, &l1) != 5 ||
		    dec.num_active_dpb_entries > V4L2_HEVC_DPB_ENTRIES_NUM_MAX ||
		    dec.num_poc_st_curr_before + dec.num_poc_st_curr_after != dec.num_active_dpb_entries ||
		    l0 > 15 || l1 > 15)
			fail("invalid HEVC RPS");
		slice.num_ref_idx_l0_active_minus1 = l0 ? l0 - 1 : 0;
		slice.num_ref_idx_l1_active_minus1 = l1 ? l1 - 1 : 0;
		for (j = 0; j < dec.num_active_dpb_entries; j++) {
			struct v4l2_hevc_dpb_entry *dpb = &dec.dpb[j];

			if (fscanf(manifest, "%u%d", &index, &dpb->pic_order_cnt_val) != 2 || index >= i)
				fail("invalid HEVC DPB entry");
			dpb->timestamp = (uint64_t)(index + 1) * 1000000000;
			if (j < dec.num_poc_st_curr_before)
				dec.poc_st_curr_before[j] = j;
			else
				dec.poc_st_curr_after[j - dec.num_poc_st_curr_before] = j;
		}
		for (j = 0; j < l0; j++)
			if (fscanf(manifest, "%hhu", &slice.ref_idx_l0[j]) != 1)
				fail("invalid HEVC list 0");
		for (j = 0; j < l1; j++)
			if (fscanf(manifest, "%hhu", &slice.ref_idx_l1[j]) != 1)
				fail("invalid HEVC list 1");

		if (sps.flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED)
			ext.count++;
		if (slice.slice_segment_addr)
			ext.count = 1;
		if ((slice.slice_type == V4L2_HEVC_SLICE_TYPE_P && pps.flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED) ||
		    (slice.slice_type == V4L2_HEVC_SLICE_TYPE_B && pps.flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED)) {
			struct v4l2_hevc_pred_weight_table *weights = &slice.pred_weight_table;

			if (fscanf(manifest, "%hhu%hhd", &weights->luma_log2_weight_denom,
				   &weights->delta_chroma_log2_weight_denom) != 2)
				fail("invalid HEVC prediction denominator");
			read_hevc_weights(manifest, l0, weights->delta_luma_weight_l0, weights->luma_offset_l0,
					  weights->delta_chroma_weight_l0, weights->chroma_offset_l0);
			read_hevc_weights(manifest, l1, weights->delta_luma_weight_l1, weights->luma_offset_l1,
					  weights->delta_chroma_weight_l1, weights->chroma_offset_l1);
		}
		queue_slice(video, media, &ext, i, bytes, hold);
		if (hold)
			continue;
		index = dequeue(video, CAPTURE, i, frame_size);
		if (index >= frames || fwrite(dst[index].addr, frame_size, 1, raw) != 1)
			fail("cannot write HEVC frame");
		printf("frame %u: POC %d, %ux%u NV21 %u bytes, CAPTURE index %u\n",
		       i++, dec.pic_order_cnt_val, width, height, frame_size, index);
	}
	IOCTL(video, VIDIOC_STREAMOFF, &output);
	IOCTL(video, VIDIOC_STREAMOFF, &capture);
	for (i = 0; i < frames; i++)
		munmap(dst[i].addr, dst[i].len);
	munmap(src.addr, src.len);
	return 0;
}

int main(int argc, char **argv)
{
	enum v4l2_buf_type output = OUTPUT, capture = CAPTURE;
	struct v4l2_capability cap = {};
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
		fail("usage: vdec-request /dev/videoN /dev/mediaN manifest slices output.nv21");

	video = open_device(argv[1]);
	media = open_device(argv[2]);
	manifest = fopen(argv[3], "r");
	stream = fopen(argv[4], "rb");
	raw = fopen(argv[5], "wb");
	if (!manifest || !stream || !raw) {
		perror("fixture/output file");
		return EXIT_FAILURE;
	}

	if (fgetc(manifest) == 'H') {
		char codec[16];

		rewind(manifest);
		if (fscanf(manifest, "%15s", codec) != 1 || strcmp(codec, "HEVC"))
			fail("invalid codec in fixture");
		run_hevc(video, media, manifest, stream, raw);
		goto close_files;
	}
	rewind(manifest);

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

	frame_size = setup_decoder(video, V4L2_PIX_FMT_H264_SLICE, width, height, frames, &src, dst);

	for (i = 0; i < frames;) {
		struct v4l2_ctrl_h264_slice_params slice = {};
		struct v4l2_ctrl_h264_decode_params dec = {};
		struct v4l2_ctrl_h264_pred_weights weights = {};
		struct v4l2_ext_control controls[] = {
			{ .id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof(sps), .ptr = &sps },
			{ .id = V4L2_CID_STATELESS_H264_PPS, .size = sizeof(pps), .ptr = &pps },
			{ .id = V4L2_CID_STATELESS_H264_SLICE_PARAMS, .size = sizeof(slice), .ptr = &slice },
			{ .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS, .size = sizeof(dec), .ptr = &dec },
			{ .id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS, .size = sizeof(weights), .ptr = &weights },
		};
		struct v4l2_ext_controls ext = {
			.which = V4L2_CTRL_WHICH_REQUEST_VAL,
			.count = sizeof(controls) / sizeof(controls[0]) - 1,
			.controls = controls,
		};
		unsigned int bytes, bits, frame_num, type, idr, ref_idc, cabac_init, deblock;
		unsigned int idx, first_mb, hold, direct_spatial;
		int delta, poc, alpha, beta;
		bool weighted;

		if (fscanf(manifest, "%u%u%u%u%u%u%u%u%d%d%d%d%u%u%u",
			   &bytes, &bits, &frame_num, &type, &idr, &ref_idc, &cabac_init,
			   &deblock, &delta, &poc, &alpha, &beta, &first_mb, &hold,
			   &direct_spatial) != 15 ||
		    bytes > src.len || fread(src.addr, 1, bytes, stream) != bytes)
			fail("invalid fixture slice");

		slice.header_bit_size = bits;
		slice.first_mb_in_slice = first_mb;
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
		weighted = V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(&pps, &slice);
		if (weighted) {
			struct v4l2_h264_weight_factors *l0 = &weights.weight_factors[0];

			if (fscanf(manifest, "%hu%hu%hd%hd%hd%hd%hd%hd",
				   &weights.luma_log2_weight_denom, &weights.chroma_log2_weight_denom,
				   &l0->luma_weight[0], &l0->luma_offset[0],
				   &l0->chroma_weight[0][0], &l0->chroma_offset[0][0],
				   &l0->chroma_weight[0][1], &l0->chroma_offset[0][1]) != 8)
				fail("invalid fixture prediction weights");
			ext.count++;
		}

		if (first_mb) {
			ext.controls = &controls[2];
			ext.count = 1;
			if (weighted) {
				controls[3] = controls[4];
				ext.count++;
			}
		} else if (i) {
			/* SPS is unchanged after the first picture. */
			ext.controls++;
			ext.count--;
		}
		queue_slice(video, media, &ext, i, bytes, hold || (test_flush && i == frames - 1));
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
close_files:
	fclose(manifest);
	fclose(stream);
	close(video);
	close(media);
	return fclose(raw) ? EXIT_FAILURE : EXIT_SUCCESS;
}
