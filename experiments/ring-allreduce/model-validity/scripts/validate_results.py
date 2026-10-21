#!/usr/bin/env python3
"""Check Stage 1 rows. Invalid runs stay in the CSVs; this script reports them."""

import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvlib


def load(name):
    path = os.path.join(mvlib.RESULTS, name)
    with open(path) as f:
        return list(csv.DictReader(f))


def main():
    runs = load("runs.csv")
    flows = load("flows.csv")
    cals = load("calibrations.csv")
    if len(cals) != 1:
        sys.stderr.write("expected one calibration row\n")
        return 1
    cal = cals[0]
    if cal.get("source") != "MV0_only":
        sys.stderr.write("calibration source is not MV0_only\n")
        return 1
    fit_wires = set(cal["message_sizes_wire"].split())
    problems = []
    by_run = {}
    for fl in flows:
        by_run.setdefault(fl["run_id"], []).append(fl)

    for row in runs:
        rid = row["run_id"]
        if row["transport"] != "paced_roce" or row["ecn_enabled"] != "0" or row["pfc_enabled"] != "0":
            problems.append(rid + " transport flags")
        if row["experiment_id"] in ("MV1", "MV2", "MV3", "MV4"):
            if row["calibration_id"] != cal["calibration_id"]:
                problems.append(rid + " calibration id")
            if row["valid"] == "1":
                if row["alpha_us"] != cal["alpha_us"] or row["beta_ns_per_byte"] != cal["beta_ns_per_byte"]:
                    problems.append(rid + " alpha/beta not from MV0")
        if row["experiment_id"] == "MV0" and row["valid"] == "1":
            if row["wire_message_bytes"] not in fit_wires:
                problems.append(rid + " MV0 size missing from fit")
            if int(row["sender_count"]) != 1:
                problems.append(rid + " MV0 was not isolated")
        if row["valid"] == "1":
            if row["drops"] != "0" or row["retransmissions"] != "0" or row["nacks"] != "0":
                problems.append(rid + " marked valid with loss")
            if row["ecn_marks"] != "0" or row["pfc_events"] != "0":
                problems.append(rid + " marked valid with ecn/pfc")
            if float(row["sim_cct_us"]) <= 0:
                problems.append(rid + " nonpositive cct")
            group = by_run.get(rid, [])
            if row["experiment_id"] == "MV4":
                fg = [fl for fl in group if fl.get("role") == "fg"]
                if len(fg) != 1:
                    problems.append(rid + " foreground flow")
                elif fg[0]["wire_bytes"] != row["wire_message_bytes"]:
                    problems.append(rid + " foreground wire bytes")
                for fl in group:
                    if float(fl["fct_us"]) < 0:
                        problems.append(rid + " negative fct")
            else:
                if len(group) != int(row["sender_count"]):
                    problems.append(rid + " flow count")
                wire = 0
                for fl in group:
                    if fl["wire_bytes"] != row["wire_message_bytes"]:
                        problems.append(rid + " flow wire bytes")
                    if float(fl["fct_us"]) < 0:
                        problems.append(rid + " negative fct")
                    wire += int(fl["wire_bytes"])
                if wire != int(row["total_wire_bytes"]):
                    problems.append(rid + " aggregate wire bytes")
        else:
            print("preserved invalid %s reason=%s" % (rid, row["invalid_reason"]))

    mv1 = [r for r in runs if r["experiment_id"] == "MV1"]
    mv2 = [r for r in runs if r["experiment_id"] == "MV2"]
    mv3 = [r for r in runs if r["experiment_id"] == "MV3"]
    if len(mv1) != 20:
        problems.append("MV1 row count %d" % len(mv1))
    if len(mv2) != 15:
        problems.append("MV2 row count %d" % len(mv2))
    if len(mv3) != 27:
        problems.append("MV3 row count %d" % len(mv3))
    mv4 = [r for r in runs if r["experiment_id"] == "MV4"]
    if len(mv4) != 9:
        problems.append("MV4 row count %d" % len(mv4))

    alpha_s = float(cal["alpha_us"]) * 1e-6
    beta = float(cal["beta_ns_per_byte"]) * 1e-9
    mv1_cct = {}
    for row in mv1:
        mv1_cct[(int(row["sender_count"]), int(row["wire_message_bytes"]))] = float(row["sim_cct_us"])
    for row in mv3:
        rid = row["run_id"]
        group = by_run.get(rid, [])
        if len(group) != 8:
            problems.append(rid + " expected 8 flows")
            continue
        group = sorted(group, key=lambda fl: int(fl["flow_id"]))
        starts = [float(fl["scheduled_start_us"]) for fl in group]
        for i in range(1, len(starts)):
            if starts[i] + 1e-9 < starts[i - 1]:
                problems.append(rid + " starts are not monotonic")
        for fl in group:
            if abs(float(fl["actual_start_us"]) - float(fl["scheduled_start_us"])) > 1e-3:
                problems.append(rid + " actual start differs from schedule")
        if row.get("sink_gaps") not in ("", "0") and row["valid"] == "1":
            problems.append(rid + " sink gaps")
        wire = int(row["wire_message_bytes"])
        n = 8
        capacity_Bps = float(row["bottleneck_gbps"]) * 1e9 / 8.0
        start_s = [s * 1e-6 for s in starts]
        finish_m2 = mvlib.fluid_finish_times(start_s, [float(wire)] * n, capacity_Bps, alpha_s)
        packets = wire // int(row["mss_bytes"])
        on_core = float(packets * mvlib.PACKET_ON_CORE_BYTES)
        finish_pkt = mvlib.fluid_finish_times(start_s, [on_core] * n, capacity_Bps, alpha_s)
        pred_m2 = (max(finish_m2) - min(start_s)) * 1e6
        pred_pkt = (max(finish_pkt) - min(start_s)) * 1e6
        if abs(pred_m2 - float(row["T_M2_active_us"])) / pred_m2 > 1e-9:
            problems.append(rid + " M2 does not match the start/capacity formula")
        if abs(pred_pkt - float(row["T_M2_packet_us"])) / pred_pkt > 1e-9:
            problems.append(rid + " M2 packet does not match the start/capacity formula")
        if abs(float(row["start_spread_over_Tiso"])) < 1e-12:
            key = (8, wire)
            if key in mv1_cct:
                base = mv1_cct[key]
                got = float(row["sim_cct_us"])
                if abs(got - base) / base > 1e-6:
                    problems.append(rid + " S=0 does not match MV1 (%.6f vs %.6f)" % (got, base))

    mv4_spec = mvlib.load_json(os.path.join(mvlib.CONFIG_DIR, "mv4.json"))
    line_Bps = None
    for row in runs:
        if row["experiment_id"] != "MV4" or row["valid"] != "1":
            continue
        rid = row["run_id"]
        if line_Bps is None:
            line_Bps = float(row["bottleneck_gbps"]) * 1e9 / 8.0
        pattern = row["background_pattern"]
        shared = row["background_shared_path"] == "1"
        iso_s = float(row["t_iso_sim_us"]) * 1e-6
        horizon_s = float(mv4_spec["horizon_over_Tfg"]) * iso_s
        if pattern == "CONSTANT" and shared:
            windows = [(0.0, horizon_s)]
            m1_Bps = line_Bps / 2.0
        elif pattern.startswith("BURSTY"):
            period_s = float(row["background_period_over_Tfg"]) * iso_s
            windows = mvlib.on_windows(
                period_s, float(row["background_duty_cycle"]),
                float(row["background_phase_fraction"]), max(horizon_s, period_s))
            m1_Bps = line_Bps / 2.0
        else:
            windows = []
            m1_Bps = line_Bps
        wire = int(row["wire_message_bytes"])
        m0 = alpha_s + beta * float(wire)
        m1 = alpha_s + float(wire) / m1_Bps
        m2 = mvlib.timeline_finish(float(wire), alpha_s, line_Bps, windows)
        packets = wire // int(row["mss_bytes"])
        m2_pkt = mvlib.timeline_finish(
            float(packets * mvlib.PACKET_ON_CORE_BYTES), alpha_s, line_Bps, windows)
        for label, pred, stored in (
            ("M0", m0, row["M0_pred_us"]),
            ("M1", m1, row["M1_average_pred_us"]),
            ("M2", m2, row["M2_timeline_pred_us"]),
            ("M2pkt", m2_pkt, row["M2_timeline_packet_pred_us"]),
        ):
            got = float(stored) * 1e-6
            if abs(pred - got) / pred > 1e-9:
                problems.append(rid + " " + label + " does not match the timeline formula")
        fg = float(row["foreground_sim_fct_us"])
        if abs(fg - float(row["sim_cct_us"])) > 1e-6:
            problems.append(rid + " foreground fct differs from sim_cct")
        if pattern == "NONE" and int(row["background_overlap_bytes"]) != 0:
            problems.append(rid + " none case has overlap bytes")
        if pattern == "DISJOINT_CONSTANT" and int(row["background_overlap_bytes"]) != 0:
            problems.append(rid + " disjoint case shares the foreground core")
    if problems:
        for item in problems:
            sys.stderr.write(item + "\n")
        return 1
    invalid = sum(r["valid"] != "1" for r in runs)
    print("validation ok  runs=%d invalid_preserved=%d calibration=%s" % (
        len(runs), invalid, cal["calibration_id"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
