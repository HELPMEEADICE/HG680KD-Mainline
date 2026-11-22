"""Prepare Main/Main10 Annex B fixtures and FFmpeg NV21/P010 reference frames."""

import argparse
from pathlib import Path

from vdec_request_fixture import START_CODE, TRACE_VALUE, ffmpeg, flags


SPS_FIELDS = (
    "log2_min_luma_coding_block_size_minus3", "log2_diff_max_min_luma_coding_block_size",
    "log2_min_luma_transform_block_size_minus2", "log2_diff_max_min_luma_transform_block_size",
    "max_transform_hierarchy_depth_inter", "max_transform_hierarchy_depth_intra",
    "log2_max_pic_order_cnt_lsb_minus4", "num_short_term_ref_pic_sets",
)
SPS_FLAGS = (
    "separate_colour_plane_flag", "scaling_list_enabled_flag", "amp_enabled_flag",
    "sample_adaptive_offset_enabled_flag", "pcm_enabled_flag", "pcm_loop_filter_disabled_flag",
    "long_term_ref_pics_present_flag", "sps_temporal_mvp_enabled_flag",
    "strong_intra_smoothing_enabled_flag",
)
PPS_FIELDS = (
    "init_qp_minus26", "pps_cb_qp_offset", "pps_cr_qp_offset", "diff_cu_qp_delta_depth",
    "log2_parallel_merge_level_minus2",
)
PPS_FLAGS = (
    "dependent_slice_segments_enabled_flag", "output_flag_present_flag", "sign_data_hiding_enabled_flag",
    "cabac_init_present_flag", "constrained_intra_pred_flag", "transform_skip_enabled_flag",
    "cu_qp_delta_enabled_flag", "pps_slice_chroma_qp_offsets_present_flag", "weighted_pred_flag",
    "weighted_bipred_flag", "transquant_bypass_enabled_flag", "tiles_enabled_flag",
    "entropy_coding_sync_enabled_flag", "loop_filter_across_tiles_enabled_flag",
    "pps_loop_filter_across_slices_enabled_flag", "deblocking_filter_override_enabled_flag",
    "pps_deblocking_filter_disabled_flag", "lists_modification_present_flag",
    "slice_segment_header_extension_present_flag", "deblocking_filter_control_present_flag",
    "uniform_spacing_flag",
)
SLICE_FLAGS = (
    "slice_sao_luma_flag", "slice_sao_chroma_flag", "slice_temporal_mvp_enabled_flag",
    "mvd_l1_zero_flag", "cabac_init_flag", "collocated_from_l0_flag", "use_integer_mv_flag",
    "slice_deblocking_filter_disabled_flag", "slice_loop_filter_across_slices_enabled_flag",
    "dependent_slice_segment_flag",
)

# H.265 default 8x8 coefficients in raster order, shared by the larger sizes.
SCALING_INTRA = (
    16, 16, 16, 16, 17, 18, 21, 24, 16, 16, 16, 16, 17, 19, 22, 25,
    16, 16, 17, 18, 20, 22, 25, 29, 16, 16, 18, 21, 24, 27, 31, 36,
    17, 17, 20, 24, 30, 35, 41, 47, 18, 19, 22, 27, 35, 44, 54, 65,
    21, 22, 25, 31, 41, 54, 70, 88, 24, 25, 29, 36, 47, 65, 88, 115,
)
SCALING_INTER = (
    16, 16, 16, 16, 17, 18, 20, 24, 16, 16, 16, 17, 18, 20, 24, 25,
    16, 16, 17, 18, 20, 24, 25, 28, 16, 17, 18, 20, 24, 25, 28, 33,
    17, 18, 20, 24, 25, 28, 33, 41, 18, 20, 24, 25, 28, 33, 41, 54,
    20, 24, 25, 28, 33, 41, 54, 71, 24, 25, 28, 33, 41, 54, 71, 91,
)


def trace_fields(stream):
    trace = ffmpeg(
        "-loglevel", "info", "-i", stream, "-c:v", "copy", "-bsf:v",
        "trace_headers", "-f", "null", "-", capture=True,
    ).stderr
    sps, pps, slices = {}, {}, []
    current = None
    for line in trace.splitlines():
        if "[trace_headers " not in line:
            continue
        if line.endswith(" Sequence Parameter Set"):
            current = sps
        elif line.endswith(" Picture Parameter Set"):
            current = pps
        elif line.endswith(" Slice Segment Header"):
            current = {}
            slices.append(current)
        elif current is not None:
            match = TRACE_VALUE.search(line)
            if match:
                pos, name, bits, value = match.groups()
                current[name] = int(value)
                current["end"] = int(pos) + len(bits)
            else:
                current = None
    return sps, pps, slices


def vcl_units(data):
    codes = list(START_CODE.finditer(data))
    for i, code in enumerate(codes):
        if data[code.end()] >> 1 > 31:
            continue
        end = codes[i + 1].start() if i + 1 < len(codes) else len(data)
        yield data[code.start():end], code.end() - code.start()


def reference_pocs(header, poc, direction, name):
    pocs = []
    previous = poc
    for i in range(header.get(f"num_{name}_pics", 0)):
        previous += direction * (header[f"delta_poc_s{int(direction > 0)}_minus1[{i}]"] + 1)
        if header[f"used_by_curr_pic_s{int(direction > 0)}_flag[{i}]"]:
            pocs.append(previous)
    return pocs


def ref_list(indices, count, row, suffix):
    if not count:
        return []
    if not indices:
        raise ValueError("inter slice has no active references")
    if row.get(f"ref_pic_list_modification_flag_{suffix}"):
        return [indices[row[f"list_entry_{suffix}[{i}]"]] for i in range(count)]
    return [indices[i % len(indices)] for i in range(count)]


def pred_weights(row, l0, l1):
    denom = row.get("luma_log2_weight_denom", 0)
    delta = row.get("delta_chroma_log2_weight_denom", 0)
    chroma_denom = denom + delta
    fields = [denom, delta]
    for count, suffix in ((l0, "l0"), (l1, "l1")):
        for i in range(count):
            fields.extend(row.get(f"{name}_{suffix}[{i}]", 0)
                          for name in ("delta_luma_weight", "luma_offset"))
            for c in range(2):
                delta_weight = row.get(f"delta_chroma_weight_{suffix}[{i}][{c}]", 0)
                # FFmpeg trace_headers calls the coded delta chroma_offset.
                delta_offset = row.get(f"chroma_offset_{suffix}[{i}][{c}]", 0)
                weight = (1 << chroma_denom) + delta_weight
                offset = 128 + delta_offset - ((128 * weight) >> chroma_denom)
                fields.extend((delta_weight, min(127, max(-128, offset))))
    return fields


def write_reference(stream, out, pocs, width, height, main10, visible=False):
    ffmpeg("-loglevel", "error", "-apply_cropping", "all" if visible else "none",
           "-i", stream, "-pix_fmt", "p010le" if main10 else "nv21", "-f", "rawvideo", out)
    size = width * height * 3 // (1 if main10 else 2)
    if out.stat().st_size != len(pocs) * size:
        raise ValueError(f"unexpected reference frame size in {out}")
    if pocs == sorted(pocs):
        return
    pixels = out.read_bytes()
    order = {poc: i for i, poc in enumerate(sorted(pocs))}
    out.write_bytes(b"".join(pixels[order[poc] * size:(order[poc] + 1) * size] for poc in pocs))


def fixture(stream, out):
    sps, pps, slices = trace_fields(stream)
    units = list(vcl_units(stream.read_bytes()))
    if not units or len(units) != len(slices):
        raise ValueError("trace does not match the VCL NAL units")
    width = sps["pic_width_in_luma_samples"]
    height = sps["pic_height_in_luma_samples"]
    depth = sps["bit_depth_luma_minus8"]
    if (sps["general_profile_idc"] not in (1, 2) or sps["chroma_format_idc"] != 1 or
            depth not in (0, 2) or sps["bit_depth_chroma_minus8"] != depth or
            sps["num_short_term_ref_pic_sets"] or
            sps.get("long_term_ref_pics_present_flag") or
            sps.get("sps_scaling_list_data_present_flag") or pps.get("pps_scaling_list_data_present_flag")):
        raise ValueError("fixture requires Main/Main10, explicit short-term RPS, and default matrices")
    main10 = depth == 2
    extension = "p010" if main10 else "nv21"
    rows = [
        ["HEVC10" if main10 else "HEVC", width, height,
         sum(row["first_slice_segment_in_pic_flag"] for row in slices)],
        [*(sps.get(key, 0) for key in SPS_FIELDS), flags(sps, SPS_FLAGS)],
        [*(pps.get(key, 0) for key in PPS_FIELDS), flags(pps, PPS_FLAGS)],
    ]
    if pps.get("tiles_enabled_flag"):
        cols = pps["num_tile_columns_minus1"]
        rows_minus1 = pps["num_tile_rows_minus1"]
        rows.append([cols, rows_minus1])
        if not pps.get("uniform_spacing_flag"):
            rows.append([*(pps[f"column_width_minus1[{i}]"] for i in range(cols)),
                         *(pps[f"row_height_minus1[{i}]"] for i in range(rows_minus1))])
    if sps.get("scaling_list_enabled_flag"):
        rows.append([16] * 96 + list(SCALING_INTRA * 3 + SCALING_INTER * 3) * 2 +
                    list(SCALING_INTRA + SCALING_INTER) + [16] * 8)
    pocs = []
    for i, ((unit, prefix), row) in enumerate(zip(units, slices)):
        first = row["first_slice_segment_in_pic_flag"]
        hold = i + 1 < len(slices) and not slices[i + 1]["first_slice_segment_in_pic_flag"]
        poc = row.get("slice_pic_order_cnt_lsb", 0)
        nal = row["nal_unit_type"]
        if row["end"] % 8 or row.get("inter_ref_pic_set_prediction_flag") or not 0 <= nal <= 31:
            raise ValueError("unsupported HEVC fixture slice header")
        if first and poc in pocs:
            raise ValueError("fixture must contain one closed GOP with no POC wrap")
        # Find each used picture by its decode-order index and preserve RPS order.
        before = reference_pocs(row, poc, -1, "negative")
        after = reference_pocs(row, poc, 1, "positive")
        refs = [(pocs.index(ref), ref) for ref in before + after]
        past = list(range(len(before)))
        future = list(range(len(before), len(refs)))
        slice_type = row["slice_type"]
        l0 = row.get("num_ref_idx_l0_active_minus1", pps["num_ref_idx_l0_default_active_minus1"]) + 1
        l1 = row.get("num_ref_idx_l1_active_minus1", pps["num_ref_idx_l1_default_active_minus1"]) + 1
        list0 = ref_list(past + future, l0 if slice_type != 2 else 0, row, "l0")
        list1 = ref_list(future + past, l1 if slice_type == 0 else 0, row, "l1")
        row.setdefault("slice_deblocking_filter_disabled_flag", pps.get("pps_deblocking_filter_disabled_flag", 0))
        row.setdefault("slice_loop_filter_across_slices_enabled_flag",
                       pps.get("pps_loop_filter_across_slices_enabled_flag", 0))
        rows.append([
            len(unit), prefix + row["end"] // 8, nal, row["nuh_temporal_id_plus1"], slice_type, poc,
            row["slice_qp_delta"], row.get("slice_cb_qp_offset", 0), row.get("slice_cr_qp_offset", 0),
            row.get("slice_beta_offset_div2", pps.get("pps_beta_offset_div2", 0)),
            row.get("slice_tc_offset_div2", pps.get("pps_tc_offset_div2", 0)),
            row.get("five_minus_max_num_merge_cand", 0), row.get("collocated_ref_idx", 0),
            row.get("slice_segment_address", 0), int(hold), flags(row, SLICE_FLAGS),
            row.get("num_entry_point_offsets", 0),
            len(refs), len(before), len(after), len(list0), len(list1),
            *(value for ref in refs for value in ref), *list0, *list1,
        ])
        if ((slice_type == 1 and pps.get("weighted_pred_flag")) or
                (slice_type == 0 and pps.get("weighted_bipred_flag"))):
            rows[-1].extend(pred_weights(row, len(list0), len(list1)))
        if not hold:
            pocs.append(poc)

    out.with_suffix(".txt").write_text(
        "".join(" ".join(map(str, row)) + "\n" for row in rows), encoding="ascii", newline="\n",
    )
    out.with_suffix(".265").write_bytes(b"".join(unit for unit, _ in units))
    write_reference(stream, out.with_suffix(f".{extension}"), pocs, width, height, main10)
    left, right, top, bottom = (
        2 * sps.get(f"conf_win_{side}_offset", 0) for side in ("left", "right", "top", "bottom")
    )
    if left or right or top or bottom:
        write_reference(stream, out.with_suffix(f".visible.{extension}"), pocs,
                        width - left - right, height - top - bottom, main10, visible=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stream", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    fixture(args.stream, args.output)
