#!/usr/bin/env python3
"""MV4: one foreground transfer, same average background, different timing.

Alpha and beta stay on the MV0 fit. Background ON intervals are real paced
flows. This script does not rerun MV0, MV1, MV2, or MV3.

Optional arguments select cases, for example:

    python3 scripts/run_mv4.py none constant
    python3 scripts/run_mv4.py bursty_medium:0
"""

import csv
import math
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvlib
from run_suite import FLOW_FIELDS, LINK_FIELDS, RUN_FIELDS, require_paced, write_csv


def read_csv(path):
    if not os.path.isfile(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def sci(value):
    return "%.16e" % value


def mss_round(raw_bytes, mss):
    packets = int(math.ceil((float(raw_bytes) / float(mss)) - 1e-12))
    if packets < 1:
        packets = 1
    return packets * mss


def case_key(case):
    if case["phase"] is None:
        return case["name"]
    return "%s:%g" % (case["name"], case["phase"])


def build_cases(spec):
    cases = [
        {"name": "none", "pattern": "NONE", "shared": 0, "period_over": 0.0,
         "phase": None, "duty": 0.0, "avg": 0.0, "kind": "none"},
        {"name": "constant", "pattern": "CONSTANT", "shared": 1, "period_over": 0.0,
         "phase": None, "duty": 1.0, "avg": float(spec["background_average_gbps"]),
         "kind": "constant"},
        {"name": "disjoint", "pattern": "DISJOINT_CONSTANT", "shared": 0, "period_over": 0.0,
         "phase": None, "duty": 1.0, "avg": float(spec["background_average_gbps"]),
         "kind": "constant"},
    ]
    labels = {0.1: "bursty_fast", 1.0: "bursty_medium", 10.0: "bursty_slow"}
    for period_over in spec["periods_over_Tfg"]:
        period_over = float(period_over)
        for phase in spec["phase_fractions"]:
            phase = float(phase)
            cases.append({
                "name": labels[period_over],
                "pattern": {0.1: "BURSTY_FAST", 1.0: "BURSTY_MEDIUM", 10.0: "BURSTY_SLOW"}[period_over],
                "shared": 1,
                "period_over": period_over,
                "phase": phase,
                "duty": float(spec["duty_cycle"]),
                "avg": float(spec["background_average_gbps"]),
                "kind": "bursty",
            })
    return cases


def invoke(common, run_id, queue, flows, trace_path):
    cmd = [
        mvlib.BINARY,
        "--run-id", run_id,
        "--mss", str(common["mss_bytes"]),
        "--sender-access-gbps", str(common["sender_access_gbps"]),
        "--receiver-access-gbps", str(common["receiver_access_gbps"]),
        "--bottleneck-gbps", str(common["bottleneck_gbps"]),
        "--propagation-us", str(common["propagation_us"]),
        "--queue-bytes", str(queue),
        "--seed", str(common["seed"]),
        "--endtime-s", str(common["endtime_s"]),
        "--trace", "full",
        "--trace-path", trace_path,
    ]
    for flow in flows:
        cmd.extend([
            "--flow",
            "%d,%d,%d,%d,%.12g,%.12g,%d" % (
                flow["src"], flow["dst"], flow["nominal"], flow["wire"],
                flow["rate"], flow["start_us"], flow["core"]),
        ])
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=300)
    record, parsed_flows, links = mvlib.parse_lines(proc.stdout)
    if proc.returncode != 0 or record is None:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(proc.stdout[-4000:])
        raise RuntimeError("simulation failed for %s" % run_id)
    return record, parsed_flows, links


def main():
    if not os.path.isfile(mvlib.BINARY):
        sys.stderr.write("missing %s\n" % mvlib.BINARY)
        sys.exit(1)
    common = mvlib.common_config()
    require_paced(common)
    cals = read_csv(os.path.join(mvlib.RESULTS, "calibrations.csv"))
    points = read_csv(os.path.join(mvlib.RESULTS, "calibration_points.csv"))
    if len(cals) != 1 or cals[0].get("source") != "MV0_only":
        sys.stderr.write("MV4 needs the existing MV0 calibration\n")
        sys.exit(1)
    cal = cals[0]
    spec = mvlib.load_json(os.path.join(mvlib.CONFIG_DIR, "mv4.json"))
    mss = int(common["mss_bytes"])
    nominal = int(spec["foreground_nominal_bytes"])
    wire = mvlib.wire_bytes(nominal, mss)
    iso = None
    for point in points:
        if point.get("valid") == "1" and int(point["wire_message_bytes"]) == wire:
            iso = float(point["t_sim_us"])
    if iso is None:
        sys.stderr.write("MV4 needs the MV0 isolated point for %d bytes\n" % wire)
        sys.exit(1)
    alpha_s = float(cal["alpha_us"]) * 1e-6
    beta = float(cal["beta_ns_per_byte"]) * 1e-9
    t_iso_s = iso * 1e-6
    line_Bps = float(common["bottleneck_gbps"]) * 1e9 / 8.0
    horizon_s = float(spec["horizon_over_Tfg"]) * t_iso_s
    queue = int(spec["queue_bytes"])
    commit = mvlib.git_commit()
    trace_dir = os.path.join(mvlib.RESULTS, "traces")
    os.makedirs(trace_dir, exist_ok=True)

    selected = set(sys.argv[1:])
    cases = build_cases(spec)
    if selected:
        cases = [c for c in cases if case_key(c) in selected or c["name"] in selected]
        if not cases:
            sys.stderr.write("no cases matched %s\n" % " ".join(sorted(selected)))
            sys.exit(1)

    rows = []
    flow_rows = []
    link_rows = []
    print("MV4 foreground %d wire bytes, T_fg_iso %.3f us, %d cases" % (wire, iso, len(cases)))
    for case in cases:
        key = case_key(case)
        run_id = "MV4-%s-seed%s" % (key.replace(":", "-p"), common["seed"])
        flows = [{
            "src": 0, "dst": 2, "nominal": nominal, "wire": wire,
            "rate": float(spec["foreground_rate_gbps"]), "start_us": 0.0, "core": 0,
        }]
        configured = []
        period_s = case["period_over"] * t_iso_s
        if case["kind"] == "constant":
            raw = case["avg"] * 1e9 / 8.0 * horizon_s
            bg_wire = mss_round(raw, mss)
            core = 0 if case["shared"] else 1
            flows.append({
                "src": 1, "dst": 3, "nominal": bg_wire, "wire": bg_wire,
                "rate": case["avg"], "start_us": 0.0, "core": core,
            })
            configured.append((0.0, horizon_s, bg_wire, case["avg"], core))
            model_windows = [(0.0, horizon_s)] if case["shared"] else []
        elif case["kind"] == "bursty":
            cover = max(horizon_s, period_s)
            windows = mvlib.on_windows(period_s, case["duty"], case["phase"], cover)
            on_s = case["duty"] * period_s
            raw = float(spec["background_on_gbps"]) * 1e9 / 8.0 * on_s
            bg_wire = mss_round(raw, mss)
            for start_s, end_s in windows:
                flows.append({
                    "src": 1, "dst": 3, "nominal": bg_wire, "wire": bg_wire,
                    "rate": float(spec["background_on_gbps"]),
                    "start_us": start_s * 1e6, "core": 0,
                })
                configured.append((start_s * 1e6, end_s * 1e6, bg_wire,
                                   float(spec["background_on_gbps"]), 0))
            model_windows = windows
        else:
            model_windows = []

        trace_path = os.path.join(trace_dir, run_id + "_queue.csv")
        record, parsed, links = invoke(common, run_id, queue, flows, trace_path)
        fg = [fl for fl in parsed if fl.get("role") == "fg"]
        if len(fg) != 1:
            raise RuntimeError("%s did not report one foreground flow" % run_id)
        fg_fct_us = float(record["fg_fct_us"])
        shared_bytes = int(record["bg_shared_bytes"])
        any_bytes = int(record["bg_any_bytes"])
        achieved = (any_bytes * 8.0 / (fg_fct_us * 1e-6) / 1e9) if fg_fct_us > 0 else 0.0

        shared_model = case["shared"] and case["kind"] != "none"
        if shared_model:
            m1_Bps = line_Bps / 2.0
        else:
            m1_Bps = line_Bps
        m0 = alpha_s + beta * float(wire)
        m1 = alpha_s + float(wire) / m1_Bps
        m2 = mvlib.timeline_finish(float(wire), alpha_s, line_Bps, model_windows)
        packets = wire // mss
        m2_pkt = mvlib.timeline_finish(
            float(packets * mvlib.PACKET_ON_CORE_BYTES), alpha_s, line_Bps, model_windows)

        row = {
            "run_id": record["run_id"],
            "experiment_id": "MV4",
            "mode": case["pattern"],
            "seed": record["seed"],
            "git_commit": commit,
            "config_hash": mvlib.config_hash(spec),
            "calibration_id": cal["calibration_id"],
            "topology": common["topology"],
            "host_count": record["sender_count"] and str(int(record["sender_count"]) + int(record["receiver_count"])),
            "sender_count": record["sender_count"],
            "receiver_count": record["receiver_count"],
            "transport": record["transport"],
            "ecn_enabled": record["ecn_enabled"],
            "pfc_enabled": record["pfc_enabled"],
            "nominal_message_bytes": record["nominal_message_bytes"],
            "wire_message_bytes": record["wire_message_bytes"],
            "packet_count": record["packet_count"],
            "mss_bytes": record["mss_bytes"],
            "total_nominal_bytes": str(sum(fl["nominal"] for fl in flows)),
            "total_wire_bytes": str(sum(fl["wire"] for fl in flows)),
            "sender_rate_gbps": sci(float(spec["foreground_rate_gbps"])),
            "aggregate_configured_rate_gbps": sci(float(spec["foreground_rate_gbps"]) + case["avg"]),
            "aggregate_achieved_rate_gbps": sci(
                ((wire + any_bytes) * 8.0 / (fg_fct_us * 1e-6) / 1e9) if fg_fct_us > 0 else 0.0),
            "offered_load_ratio_rho": sci((float(spec["foreground_rate_gbps"]) + case["avg"]) / float(common["bottleneck_gbps"])),
            "sender_access_gbps": record["sender_access_gbps"],
            "receiver_access_gbps": record["receiver_access_gbps"],
            "bottleneck_gbps": record["bottleneck_gbps"],
            "bottleneck_queue_bytes": record["bottleneck_queue_bytes"],
            "alpha_us": cal["alpha_us"],
            "beta_ns_per_byte": cal["beta_ns_per_byte"],
            "sim_cct_us": sci(fg_fct_us),
            "mean_flow_fct_us": record["mean_flow_fct_us"],
            "max_flow_fct_us": record["max_flow_fct_us"],
            "peak_queue_bytes": record["peak_queue_bytes"],
            "mean_queue_bytes": record["mean_queue_bytes"],
            "queue_integral_byte_us": record["queue_integral_byte_us"],
            "bottleneck_utilization": record["bottleneck_utilization"],
            "ecn_marks": record["ecn_marks"],
            "pfc_events": record["pfc_events"],
            "drops": record["drops"],
            "retransmissions": record["retransmissions"],
            "nacks": record["nacks"],
            "sink_gaps": record.get("sink_gaps", "0"),
            "valid": record["valid"],
            "invalid_reason": record["invalid_reason"],
            "t_iso_fit_us": sci((alpha_s + beta * float(wire)) * 1e6),
            "t_iso_sim_us": sci(iso),
            "background_pattern": case["pattern"],
            "background_shared_path": str(case["shared"]),
            "background_period_us": sci(period_s * 1e6),
            "background_period_over_Tfg": sci(case["period_over"]),
            "background_phase_fraction": "" if case["phase"] is None else sci(case["phase"]),
            "background_duty_cycle": sci(case["duty"]),
            "background_configured_avg_gbps": sci(case["avg"]),
            "background_achieved_avg_gbps": sci(achieved),
            "background_overlap_bytes": str(shared_bytes),
            "background_overlap_time_us": record["bg_shared_busy_us"],
            "foreground_sim_fct_us": sci(fg_fct_us),
            "M0_pred_us": sci(m0 * 1e6),
            "M1_average_pred_us": sci(m1 * 1e6),
            "M2_timeline_pred_us": sci(m2 * 1e6),
            "M2_timeline_packet_pred_us": sci(m2_pkt * 1e6),
        }
        if fg_fct_us > 0 and m1 > 0 and m2 > 0 and m2_pkt > 0:
            row["ratio_sim_M0"] = sci(fg_fct_us / (m0 * 1e6))
            row["ratio_sim_M1_average"] = sci(fg_fct_us / (m1 * 1e6))
            row["ratio_sim_M2_timeline"] = sci(fg_fct_us / (m2 * 1e6))
            row["ratio_sim_M2_timeline_packet"] = sci(fg_fct_us / (m2_pkt * 1e6))
        rows.append(row)

        for fl in parsed:
            fl["run_id"] = record["run_id"]
            flow_rows.append(fl)
        for lk in links:
            lk["run_id"] = record["run_id"]
            link_rows.append(lk)

        timeline_path = os.path.join(trace_dir, run_id + "_background_timeline.csv")
        with open(timeline_path, "w") as out:
            out.write("kind,index,start_us,end_us,wire_bytes,rate_gbps,core\n")
            for index, item in enumerate(configured):
                out.write("configured,%d,%.16e,%.16e,%d,%.16e,%d\n" % (
                    index, item[0], item[1], item[2], item[3], item[4]))
            bg_index = 0
            for fl in parsed:
                if fl.get("role") == "fg":
                    continue
                out.write("actual,%d,%.16e,%.16e,%s,%.16e,%s\n" % (
                    bg_index, float(fl["actual_start_us"]), float(fl["finish_us"]),
                    fl["wire_bytes"], float(fl["configured_rate_gbps"]), fl["core"]))
                bg_index += 1

        print("  %s valid=%s fg_us=%.3f achieved_bg=%.3f Gbps overlap_bytes=%d M1=%.4f M2=%.4f M2pkt=%.4f" % (
            run_id, record["valid"], fg_fct_us, achieved, shared_bytes,
            float(row["ratio_sim_M1_average"] or 0),
            float(row["ratio_sim_M2_timeline"] or 0),
            float(row["ratio_sim_M2_timeline_packet"] or 0)))

    runs_path = os.path.join(mvlib.RESULTS, "runs.csv")
    flows_path = os.path.join(mvlib.RESULTS, "flows.csv")
    links_path = os.path.join(mvlib.RESULTS, "links.csv")
    new_ids = set(r["run_id"] for r in rows)
    kept_runs = [r for r in read_csv(runs_path) if r.get("run_id") not in new_ids]
    kept_ids = set(r["run_id"] for r in kept_runs)
    kept_flows = [fl for fl in read_csv(flows_path) if fl.get("run_id") in kept_ids]
    kept_links = [lk for lk in read_csv(links_path) if lk.get("run_id") in kept_ids]
    write_csv(runs_path, RUN_FIELDS, kept_runs + rows)
    write_csv(flows_path, FLOW_FIELDS, kept_flows + flow_rows)
    write_csv(links_path, LINK_FIELDS, kept_links + link_rows)
    print("Wrote %s" % runs_path)
    print("MV4 valid %d / %d" % (sum(r["valid"] == "1" for r in rows), len(rows)))


if __name__ == "__main__":
    main()
