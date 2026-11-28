"""Prepare progressive I/P/B fixtures for the V4L2 request test."""

import argparse
from pathlib import Path
import re
import subprocess


TRACE_VALUE = re.compile(r"\]\s+(\d+)\s+([\w\[\]]+)\s+([01]+)\s+=\s+(-?\d+)")
START_CODE = re.compile(rb"\x00\x00\x00?\x01")


def ffmpeg(*args, capture=False):
    return subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostdin", "-y", *map(str, args)],
        check=True,
        stderr=subprocess.PIPE if capture else None,
        text=True,
        encoding="utf-8",
    )


def parse_trace(trace):
    sps, pps, slices = {}, {}, []
    current = {}
    for line in trace.splitlines():
        if "[trace_headers " not in line:
            continue
        if line.endswith(" Sequence Parameter Set"):
            current = sps
        elif line.endswith(" Picture Parameter Set"):
            current = pps
        elif line.endswith(" Slice Header"):
            current = {}
            slices.append(current)
        else:
            match = TRACE_VALUE.search(line)
            if match:
                pos, name, bits, value = match.groups()
                current[name] = int(value)
                current["end"] = int(pos) + len(bits)
    return sps, pps, slices


def vcl_units(data):
    codes = list(START_CODE.finditer(data))
    for i, code in enumerate(codes):
        end = codes[i + 1].start() if i + 1 < len(codes) else len(data)
        if data[code.end()] & 31 in (1, 5):
            yield data[code.start():end]


def flags(fields, names):
    return sum((1 << i) for i, name in enumerate(names) if fields.get(name, 0))


def fixture(stream, out, *, all_idr=False):
    log = ffmpeg(
        "-loglevel", "info", "-i", stream, "-c:v", "copy",
        "-bsf:v", "trace_headers", "-f", "null", "-", capture=True,
    ).stderr
    sps, pps, slices = parse_trace(log)
    units = list(vcl_units(stream.read_bytes()))
    if not units or len(units) != len(slices):
        raise ValueError("trace does not match the VCL NAL units")
    if (sps["pic_order_cnt_type"] not in (0, 2) or
            not sps["frame_mbs_only_flag"] or sps["max_num_ref_frames"] > 2 or
            pps["weighted_bipred_idc"] or
            pps["num_ref_idx_l0_default_active_minus1"] or
            pps["num_ref_idx_l1_default_active_minus1"]):
        raise ValueError("fixture requires progressive I/P/B with single-entry lists and unweighted B")

    width = (sps["pic_width_in_mbs_minus1"] + 1) * 16
    height = (sps["pic_height_in_map_units_minus1"] + 1) * 16
    constraints = flags(sps, [f"constraint_set{i}_flag" for i in range(6)])
    sps_flags = flags(sps, [
        "separate_colour_plane_flag", "qpprime_y_zero_transform_bypass_flag",
        "delta_pic_order_always_zero_flag", "gaps_in_frame_num_allowed_flag",
        "frame_mbs_only_flag", "mb_adaptive_frame_field_flag", "direct_8x8_inference_flag",
    ])
    pps_flags = flags(pps, [
        "entropy_coding_mode_flag", "bottom_field_pic_order_in_frame_present_flag",
        "weighted_pred_flag", "deblocking_filter_control_present_flag",
        "constrained_intra_pred_flag", "redundant_pic_cnt_present_flag",
        "transform_8x8_mode_flag", "pic_scaling_matrix_present_flag",
    ])
    rows = [[
        width, height, sum(row["first_mb_in_slice"] == 0 for row in slices), sps["profile_idc"], sps["level_idc"], constraints,
        sps["log2_max_frame_num_minus4"], sps["pic_order_cnt_type"],
        sps.get("log2_max_pic_order_cnt_lsb_minus4", 0),
        sps["max_num_ref_frames"], sps_flags, pps_flags, pps["pic_init_qp_minus26"],
        pps["chroma_qp_index_offset"],
        pps.get("second_chroma_qp_index_offset", pps["chroma_qp_index_offset"]),
    ]]
    pocs = []
    for i, (unit, fields) in enumerate(zip(units, slices)):
        hold = i + 1 < len(slices) and slices[i + 1]["first_mb_in_slice"] != 0
        if (fields["slice_type"] % 5 not in (0, 1, 2)
                or fields.get("ref_pic_list_modification_flag_l0")
                or fields.get("ref_pic_list_modification_flag_l1")
                or fields.get("num_ref_idx_l0_active_minus1", 0)
                or fields.get("num_ref_idx_l1_active_minus1", 0)):
            raise ValueError("unexpected slice type, order or reference list")
        idr = fields["nal_unit_type"] == 5
        if all_idr and (not idr or fields["first_mb_in_slice"] or hold):
            raise ValueError("expected one IDR slice per frame")
        if not all_idr and idr and not fields["first_mb_in_slice"] and pocs:
            raise ValueError("fixture must contain one closed GOP")
        if fields["slice_type"] % 5 == 0 and not fields["nal_ref_idc"]:
            raise ValueError("fixture requires each P picture to be a reference")
        poc = fields.get("pic_order_cnt_lsb", 2 * fields["frame_num"])
        if not fields["first_mb_in_slice"]:
            pocs.append(poc)
        rows.append([
            len(unit), fields["end"], fields["frame_num"], fields["slice_type"] % 5,
            int(idr), fields["nal_ref_idc"], fields.get("cabac_init_idc", 0),
            fields.get("disable_deblocking_filter_idc", 0), fields["slice_qp_delta"],
            poc, fields.get("slice_alpha_c0_offset_div2", 0),
            fields.get("slice_beta_offset_div2", 0), fields["first_mb_in_slice"], int(hold),
            fields.get("direct_spatial_mv_pred_flag", 0),
        ])
        if pps["weighted_pred_flag"] and fields["slice_type"] % 5 == 0:
            luma_denom = fields["luma_log2_weight_denom"]
            chroma_denom = fields["chroma_log2_weight_denom"]
            rows[-1].extend([
                luma_denom, chroma_denom,
                fields.get("luma_weight_l0[0]", 1 << luma_denom),
                fields.get("luma_offset_l0[0]", 0),
                fields.get("chroma_weight_l0[0][0]", 1 << chroma_denom),
                fields.get("chroma_offset_l0[0][0]", 0),
                fields.get("chroma_weight_l0[0][1]", 1 << chroma_denom),
                fields.get("chroma_offset_l0[0][1]", 0),
            ])
    out.with_suffix(".txt").write_text(
        "".join(" ".join(map(str, row)) + "\n" for row in rows), encoding="ascii", newline="\n",
    )
    out.with_suffix(".264").write_bytes(b"".join(units))
    raw = out.with_suffix(".nv21")
    # CAPTURE contains the coded picture, including any SPS crop margins.
    strict = ("-xerror", "-err_detect", "explode") if all_idr else ()
    ffmpeg("-loglevel", "error", *strict, "-apply_cropping", 0, "-i", stream,
           "-pix_fmt", "nv21", "-f", "rawvideo", raw)
    frame_size = width * height * 3 // 2
    if raw.stat().st_size != len(pocs) * frame_size:
        raise ValueError("software reference does not match the coded dimensions")
    if all_idr:
        return sps, pps, slices
    if len(pocs) != len(set(pocs)):
        raise ValueError("reference fixture POCs must be unique")
    if pocs != sorted(pocs):
        # FFmpeg writes display order; the request tester writes decode order.
        display_index = {poc: i for i, poc in enumerate(sorted(pocs))}
        data = raw.read_bytes()
        raw.write_bytes(b"".join(
            data[i * frame_size:(i + 1) * frame_size]
            for i in (display_index[poc] for poc in pocs)
        ))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stream", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    fixture(args.stream, args.output)


if __name__ == "__main__":
    main()
