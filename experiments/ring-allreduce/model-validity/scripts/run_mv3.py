#!/usr/bin/env python3
"""MV3: eight paced flows on the MV1 dumbbell, with evenly staggered starts.

Alpha and beta come from the existing MV0 calibration. This script does not
refit them and does not rerun MV0, MV1, or MV2.
"""

import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvlib
from run_suite import (
    FLOW_FIELDS, LINK_FIELDS, RUN_FIELDS, apply_models, blank_row, invoke, require_paced, write_csv,
)


def read_csv(path):
    if not os.path.isfile(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def starts_for(spread_us, n):
    if n < 2 or spread_us == 0:
        return [0.0] * n
    return [i * spread_us / float(n - 1) for i in range(n)]


def sci(value):
    return "%.16e" % value


def main():
    if not os.path.isfile(mvlib.BINARY):
        sys.stderr.write("missing %s\n" % mvlib.BINARY)
        sys.exit(1)
    common = mvlib.common_config()
    require_paced(common)
    cals = read_csv(os.path.join(mvlib.RESULTS, "calibrations.csv"))
    points = read_csv(os.path.join(mvlib.RESULTS, "calibration_points.csv"))
    if len(cals) != 1 or cals[0].get("source") != "MV0_only":
        sys.stderr.write("MV3 needs the existing MV0 calibration\n")
        sys.exit(1)
    cal = cals[0]
    alpha_s = float(cal["alpha_us"]) * 1e-6
    beta = float(cal["beta_ns_per_byte"]) * 1e-9
    iso_sim = {}
    for point in points:
        if point.get("valid") == "1":
            iso_sim[int(point["wire_message_bytes"])] = float(point["t_sim_us"])

    spec = mvlib.load_json(os.path.join(mvlib.CONFIG_DIR, "mv3.json"))
    n = int(spec["sender_count"])
    rate = float(spec["sender_rate_gbps"])
    mss = int(common["mss_bytes"])
    commit = mvlib.git_commit()
    trace_dir = os.path.join(mvlib.RESULTS, "traces")
    os.makedirs(trace_dir, exist_ok=True)

    runs_path = os.path.join(mvlib.RESULTS, "runs.csv")
    already = set(r["run_id"] for r in read_csv(runs_path) if r.get("experiment_id") == "MV3")
    jobs = []
    for nominal in spec["nominal_message_bytes"]:
        nominal = int(nominal)
        wire = mvlib.wire_bytes(nominal, mss)
        t_iso_s = alpha_s + beta * float(wire)
        for fraction in spec["start_spread_over_Tiso"]:
            fraction = float(fraction)
            spread_us = fraction * t_iso_s * 1e6
            starts = starts_for(spread_us, n)
            run_id = "MV3-n%d-w%d-s%.2f-seed%s" % (n, wire, fraction, common["seed"])
            if run_id in already:
                continue
            full = (nominal == 8388608 and abs(fraction - 1.0) < 1e-12)
            jobs.append({
                "run_id": run_id,
                "nominal": nominal,
                "wire": wire,
                "fraction": fraction,
                "spread_us": spread_us,
                "starts": starts,
                "t_iso_s": t_iso_s,
                "queue": mvlib.queue_bytes(n, wire, mss),
                "trace_path": os.path.join(trace_dir, run_id + "_queue.csv") if full else None,
            })

    print("MV3 staggered starts, %d runs" % len(jobs))
    rows = []
    flow_rows = []
    link_rows = []
    for index, job in enumerate(jobs):
        record, flows, links = invoke(
            common, job["run_id"], n, job["nominal"], job["wire"], rate, job["queue"],
            starts_us=job["starts"], trace_path=job["trace_path"])
        row = blank_row(common, commit, "MV3", "evenly_staggered", cal["calibration_id"],
                        n, job["nominal"], job["wire"], rate, job["queue"], record)
        apply_models(row, alpha_s, beta)
        row["alpha_us"] = cal["alpha_us"]
        row["beta_ns_per_byte"] = cal["beta_ns_per_byte"]

        starts = job["starts"]
        capacity_Bps = float(common["bottleneck_gbps"]) * 1e9 / 8.0
        start_s = [s * 1e-6 for s in starts]
        payload = [float(job["wire"])] * n
        packets = job["wire"] // mss
        on_core = [float(packets * mvlib.PACKET_ON_CORE_BYTES)] * n
        finish_m2 = mvlib.fluid_finish_times(start_s, payload, capacity_Bps, alpha_s)
        finish_pkt = mvlib.fluid_finish_times(start_s, on_core, capacity_Bps, alpha_s)
        t_m1_flow = alpha_s + float(job["wire"]) / (capacity_Bps / float(n))
        t_m1_makespan = max(start_s[i] + t_m1_flow for i in range(n)) - min(start_s)
        t_m2_makespan = max(finish_m2) - min(start_s)
        t_pkt_makespan = max(finish_pkt) - min(start_s)
        makespan_us = float(row["sim_cct_us"])

        actual_starts = []
        finishes = []
        fcts = []
        for fl in flows:
            fl["run_id"] = record["run_id"]
            fid = int(fl["flow_id"])
            scheduled = starts[fid]
            fl["scheduled_start_us"] = sci(scheduled)
            if not fl.get("actual_start_us"):
                fl["actual_start_us"] = fl.get("start_us", sci(scheduled))
            actual = float(fl["actual_start_us"])
            finish = float(fl["finish_us"])
            actual_starts.append(actual)
            finishes.append(finish)
            fcts.append(float(fl["fct_us"]))
            fl["predicted_finish_M0_us"] = sci((start_s[fid] + job["t_iso_s"]) * 1e6)
            fl["predicted_finish_M1_us"] = sci((start_s[fid] + t_m1_flow) * 1e6)
            fl["predicted_finish_M2_us"] = sci(finish_m2[fid] * 1e6)
            fl["predicted_finish_M2_packet_us"] = sci(finish_pkt[fid] * 1e6)
            flow_rows.append(fl)
        for lk in links:
            lk["run_id"] = record["run_id"]
            link_rows.append(lk)

        timeline, max_active, mean_active, overlap, frac_max = mvlib.active_timeline(actual_starts, finishes)
        trace_path = os.path.join(trace_dir, job["run_id"] + "_active_flows.csv")
        with open(trace_path, "w") as f:
            f.write("time_us,active_flow_count\n")
            for time, count in timeline:
                f.write("%.16e,%d\n" % (time, count))

        earliest = min(actual_starts) if actual_starts else 0.0
        latest = max(actual_starts) if actual_starts else 0.0
        first_finish = min(finishes) if finishes else 0.0
        last_finish = max(finishes) if finishes else 0.0
        # Contention tail uses configured starts and the isolated fit, not MV3 times.
        iso_arrival = max(start_s[i] + job["t_iso_s"] for i in range(n)) * 1e6
        contention_tail = last_finish - iso_arrival

        row["start_pattern"] = spec["start_pattern"]
        row["start_spread_us"] = sci(job["spread_us"])
        row["start_spread_over_Tiso"] = sci(job["fraction"])
        row["earliest_start_us"] = sci(earliest)
        row["latest_start_us"] = sci(latest)
        row["first_finish_us"] = sci(first_finish)
        row["last_finish_us"] = sci(last_finish)
        row["sim_makespan_us"] = sci(makespan_us)
        row["tail_after_last_start_us"] = sci(last_finish - latest)
        row["contention_tail_us"] = sci(contention_tail)
        row["median_flow_fct_us"] = sci(mvlib.percentile(fcts, 0.5))
        row["p95_flow_fct_us"] = sci(mvlib.percentile(fcts, 0.95))
        row["max_active_flows"] = str(max_active)
        row["time_weighted_mean_active_flows"] = sci(mean_active)
        row["total_overlap_time_us"] = sci(overlap)
        row["fraction_time_at_max_concurrency"] = sci(frac_max)
        row["T_M1_static_us"] = sci(t_m1_makespan * 1e6)
        row["T_M2_active_us"] = sci(t_m2_makespan * 1e6)
        row["T_M2_packet_us"] = sci(t_pkt_makespan * 1e6)
        if makespan_us > 0 and row["valid"] == "1":
            row["ratio_sim_M1_static"] = sci(makespan_us / (t_m1_makespan * 1e6))
            row["ratio_sim_M2_active"] = sci(makespan_us / (t_m2_makespan * 1e6))
            row["ratio_sim_M2_packet"] = sci(makespan_us / (t_pkt_makespan * 1e6))
        row["t_iso_fit_us"] = sci(job["t_iso_s"] * 1e6)
        row["t_iso_sim_us"] = sci(iso_sim[job["wire"]]) if job["wire"] in iso_sim else ""
        rows.append(row)
        print("  %s valid=%s makespan_us=%.3f max_active=%d mean_active=%.2f M2pkt=%.4f" % (
            job["run_id"], row["valid"], makespan_us, max_active, mean_active,
            float(row["ratio_sim_M2_packet"] or 0)))
        if index + 1 == len(jobs) or (index + 1) % 6 == 0:
            print("  finished %d/%d" % (index + 1, len(jobs)))

    flows_path = os.path.join(mvlib.RESULTS, "flows.csv")
    links_path = os.path.join(mvlib.RESULTS, "links.csv")
    prior_runs = read_csv(runs_path)
    prior_flows = read_csv(flows_path)
    new_ids = set(r["run_id"] for r in rows)
    kept_runs = [r for r in prior_runs if r.get("run_id") not in new_ids]
    by_flow = {}
    for fl in prior_flows:
        by_flow.setdefault(fl["run_id"], []).append(fl)
    for row in kept_runs:
        if row.get("experiment_id") != "MV3" or row.get("fraction_time_at_max_concurrency"):
            continue
        group = by_flow.get(row["run_id"], [])
        if not group:
            continue
        _, _, _, _, frac_max = mvlib.active_timeline(
            [float(fl["actual_start_us"]) for fl in group],
            [float(fl["finish_us"]) for fl in group])
        row["fraction_time_at_max_concurrency"] = sci(frac_max)
    kept_ids = set(r["run_id"] for r in kept_runs)
    kept_flows = [fl for fl in prior_flows if fl.get("run_id") in kept_ids]
    kept_links = [lk for lk in read_csv(links_path) if lk.get("run_id") in kept_ids]
    write_csv(runs_path, RUN_FIELDS, kept_runs + rows)
    write_csv(flows_path, FLOW_FIELDS, kept_flows + flow_rows)
    write_csv(links_path, LINK_FIELDS, kept_links + link_rows)
    print("Wrote %s" % runs_path)
    print("MV3 valid %d / %d" % (sum(r["valid"] == "1" for r in rows), len(rows)))


if __name__ == "__main__":
    main()
