#!/usr/bin/env python3
"""Run MV0, then MV1 and MV2 against that isolated calibration.

Alpha and beta are fit only on the one-sender MV0 sweeps. MV1 and MV2
never enter the fit.
"""

import csv
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvlib

WORKERS = 4

RUN_FIELDS = [
    "run_id", "experiment_id", "mode", "seed", "git_commit", "config_hash", "calibration_id",
    "topology", "host_count", "sender_count", "receiver_count",
    "transport", "ecn_enabled", "pfc_enabled",
    "nominal_message_bytes", "wire_message_bytes", "packet_count", "mss_bytes",
    "total_nominal_bytes", "total_wire_bytes",
    "sender_rate_gbps", "aggregate_configured_rate_gbps", "aggregate_achieved_rate_gbps",
    "offered_load_ratio_rho",
    "sender_access_gbps", "receiver_access_gbps", "bottleneck_gbps", "bottleneck_queue_bytes",
    "alpha_us", "beta_ns_per_byte",
    "T_M0_us", "T_M1_capacity_us", "T_M1_beta_us",
    "sim_cct_us",
    "ratio_sim_M0", "ratio_sim_M1_capacity", "ratio_sim_M1_beta",
    "mean_flow_fct_us", "max_flow_fct_us",
    "peak_queue_bytes", "mean_queue_bytes", "queue_integral_byte_us",
    "bottleneck_utilization",
    "ecn_marks", "pfc_events", "drops", "retransmissions", "nacks", "sink_gaps",
    "valid", "invalid_reason",
    "start_pattern", "start_spread_us", "start_spread_over_Tiso",
    "earliest_start_us", "latest_start_us", "first_finish_us", "last_finish_us",
    "sim_makespan_us", "tail_after_last_start_us", "contention_tail_us",
    "median_flow_fct_us", "p95_flow_fct_us",
    "max_active_flows", "time_weighted_mean_active_flows", "total_overlap_time_us",
    "fraction_time_at_max_concurrency",
    "T_M1_static_us", "T_M2_active_us", "T_M2_packet_us",
    "ratio_sim_M1_static", "ratio_sim_M2_active", "ratio_sim_M2_packet",
    "t_iso_fit_us", "t_iso_sim_us",
    "background_pattern", "background_shared_path",
    "background_period_us", "background_period_over_Tfg", "background_phase_fraction",
    "background_duty_cycle", "background_configured_avg_gbps", "background_achieved_avg_gbps",
    "background_overlap_bytes", "background_overlap_time_us",
    "foreground_sim_fct_us",
    "M0_pred_us", "M1_average_pred_us", "M2_timeline_pred_us", "M2_timeline_packet_pred_us",
    "ratio_sim_M1_average", "ratio_sim_M2_timeline", "ratio_sim_M2_timeline_packet",
]

FLOW_FIELDS = [
    "run_id", "flow_id", "src", "dst", "nominal_bytes", "wire_bytes",
    "configured_rate_gbps", "start_us", "scheduled_start_us", "actual_start_us",
    "finish_us", "fct_us", "achieved_throughput_gbps", "drops", "retransmissions",
    "predicted_finish_M0_us", "predicted_finish_M1_us",
    "predicted_finish_M2_us", "predicted_finish_M2_packet_us",
    "role", "core",
]

LINK_FIELDS = [
    "run_id", "link_id", "capacity_gbps", "propagation_us", "bytes_served",
    "peak_queue_bytes", "ecn_marks", "drops", "bottleneck",
]

POINT_FIELDS = [
    "calibration_id", "nominal_message_bytes", "wire_message_bytes",
    "t_sim_us", "t_fit_us", "residual_us", "valid",
]


def write_csv(path, fields, rows):
    with open(path, "w") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore", restval="")
        w.writeheader()
        for row in rows:
            w.writerow(row)


def require_paced(common):
    if common.get("transport") != "paced_roce":
        sys.stderr.write("Stage 1 only runs paced_roce\n")
        sys.exit(1)
    if common.get("ecn_enabled") or common.get("pfc_enabled"):
        sys.stderr.write("Stage 1 keeps ECN and PFC off\n")
        sys.exit(1)


def invoke(common, run_id, senders, nominal, wire, rate, queue, starts_us=None, trace_path=None):
    cmd = [
        mvlib.BINARY,
        "--run-id", run_id,
        "--senders", str(senders),
        "--nominal-bytes", str(nominal),
        "--wire-bytes", str(wire),
        "--mss", str(common["mss_bytes"]),
        "--sender-rate-gbps", "%.16g" % rate,
        "--sender-access-gbps", str(common["sender_access_gbps"]),
        "--receiver-access-gbps", str(common["receiver_access_gbps"]),
        "--bottleneck-gbps", str(common["bottleneck_gbps"]),
        "--propagation-us", str(common["propagation_us"]),
        "--queue-bytes", str(queue),
        "--seed", str(common["seed"]),
        "--endtime-s", str(common["endtime_s"]),
        "--trace", "full" if trace_path else common.get("trace_level", "summary"),
    ]
    if starts_us:
        cmd.extend(["--starts-us", ",".join("%.16g" % s for s in starts_us)])
    if trace_path:
        cmd.extend(["--trace-path", trace_path])
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=300)
    record, flows, links = mvlib.parse_lines(proc.stdout)
    if proc.returncode != 0 or record is None:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(proc.stdout)
        record = {
            "run_id": run_id,
            "sender_count": str(senders),
            "receiver_count": str(senders),
            "nominal_message_bytes": str(nominal),
            "wire_message_bytes": str(wire),
            "packet_count": str(wire // int(common["mss_bytes"])),
            "mss_bytes": str(common["mss_bytes"]),
            "sender_rate_gbps": "%.16e" % rate,
            "sender_access_gbps": str(common["sender_access_gbps"]),
            "receiver_access_gbps": str(common["receiver_access_gbps"]),
            "bottleneck_gbps": str(common["bottleneck_gbps"]),
            "bottleneck_queue_bytes": str(queue),
            "propagation_us": str(common["propagation_us"]),
            "seed": str(common["seed"]),
            "transport": "paced_roce",
            "ecn_enabled": "0",
            "pfc_enabled": "0",
            "sim_cct_us": "0",
            "mean_flow_fct_us": "0",
            "max_flow_fct_us": "0",
            "peak_queue_bytes": "0",
            "mean_queue_bytes": "0",
            "queue_integral_byte_us": "0",
            "bottleneck_utilization": "0",
            "ecn_marks": "0",
            "pfc_events": "0",
            "drops": "0",
            "retransmissions": "0",
            "nacks": "0",
            "valid": "0",
            "invalid_reason": "simulator_failed",
            "bottleneck_traversed": "0",
            "packets_ok": "0",
        }
        flows, links = [], []
    return record, flows, links


def blank_row(common, commit, experiment, mode, calibration_id, senders, nominal, wire, rate, queue, record):
    resolved = dict(common)
    resolved.update({
        "experiment_id": experiment,
        "mode": mode,
        "sender_count": senders,
        "nominal_message_bytes": nominal,
        "wire_message_bytes": wire,
        "sender_rate_gbps": rate,
        "bottleneck_queue_bytes": queue,
    })
    cct = float(record["sim_cct_us"])
    total_wire = wire * senders
    agg_cfg = rate * senders
    agg_ach = (total_wire * 8.0 / (cct * 1e-6) / 1e9) if cct > 0 else 0.0
    rho = agg_cfg / float(common["bottleneck_gbps"])
    row = {
        "run_id": record["run_id"],
        "experiment_id": experiment,
        "mode": mode,
        "seed": record["seed"],
        "git_commit": commit,
        "config_hash": mvlib.config_hash(resolved),
        "calibration_id": calibration_id,
        "topology": common["topology"],
        "host_count": str(2 * senders),
        "sender_count": record["sender_count"],
        "receiver_count": record["receiver_count"],
        "transport": record["transport"],
        "ecn_enabled": record["ecn_enabled"],
        "pfc_enabled": record["pfc_enabled"],
        "nominal_message_bytes": record["nominal_message_bytes"],
        "wire_message_bytes": record["wire_message_bytes"],
        "packet_count": record["packet_count"],
        "mss_bytes": record["mss_bytes"],
        "total_nominal_bytes": str(nominal * senders),
        "total_wire_bytes": str(total_wire),
        "sender_rate_gbps": "%.16e" % rate,
        "aggregate_configured_rate_gbps": "%.16e" % agg_cfg,
        "aggregate_achieved_rate_gbps": "%.16e" % agg_ach,
        "offered_load_ratio_rho": "%.16e" % rho,
        "sender_access_gbps": record["sender_access_gbps"],
        "receiver_access_gbps": record["receiver_access_gbps"],
        "bottleneck_gbps": record["bottleneck_gbps"],
        "bottleneck_queue_bytes": record["bottleneck_queue_bytes"],
        "alpha_us": "",
        "beta_ns_per_byte": "",
        "T_M0_us": "",
        "T_M1_capacity_us": "",
        "T_M1_beta_us": "",
        "sim_cct_us": record["sim_cct_us"],
        "ratio_sim_M0": "",
        "ratio_sim_M1_capacity": "",
        "ratio_sim_M1_beta": "",
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
        "sink_gaps": record.get("sink_gaps", ""),
        "valid": record["valid"],
        "invalid_reason": record["invalid_reason"],
        "start_pattern": "synchronized",
        "start_spread_us": "0",
        "start_spread_over_Tiso": "0",
    }
    return row


def apply_models(row, alpha_s, beta):
    if row["valid"] != "1":
        return
    t0, t_cap, t_beta = mvlib.predictions(
        alpha_s, beta, int(row["wire_message_bytes"]), int(row["sender_count"]),
        float(row["bottleneck_gbps"]))
    cct = float(row["sim_cct_us"]) * 1e-6
    row["alpha_us"] = "%.16e" % (alpha_s * 1e6)
    row["beta_ns_per_byte"] = "%.16e" % (beta * 1e9)
    row["T_M0_us"] = "%.16e" % (t0 * 1e6)
    row["T_M1_capacity_us"] = "%.16e" % (t_cap * 1e6)
    row["T_M1_beta_us"] = "%.16e" % (t_beta * 1e6)
    row["ratio_sim_M0"] = "%.16e" % (cct / t0)
    row["ratio_sim_M1_capacity"] = "%.16e" % (cct / t_cap)
    row["ratio_sim_M1_beta"] = "%.16e" % (cct / t_beta)


def run_jobs(common, jobs):
    results = [None] * len(jobs)

    def one(job):
        return invoke(common, job["run_id"], job["senders"], job["nominal"],
                      job["wire"], job["rate"], job["queue"])

    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        futs = []
        for idx, job in enumerate(jobs):
            futs.append((idx, pool.submit(one, job)))
        done = 0
        for idx, fut in futs:
            results[idx] = fut.result()
            done += 1
            if done % 5 == 0 or done == len(jobs):
                print("  finished %d/%d" % (done, len(jobs)))
    return results


def main():
    if not os.path.isfile(mvlib.BINARY):
        sys.stderr.write("missing %s\n" % mvlib.BINARY)
        sys.exit(1)
    os.makedirs(mvlib.RESULTS, exist_ok=True)
    common = mvlib.common_config()
    require_paced(common)
    commit = mvlib.git_commit()
    mss = int(common["mss_bytes"])
    line = float(common["bottleneck_gbps"])

    mv0 = mvlib.load_json(os.path.join(mvlib.CONFIG_DIR, "mv0.json"))
    print("MV0 isolated calibration")
    jobs = []
    for nominal in mv0["nominal_message_bytes"]:
        wire = mvlib.wire_bytes(nominal, mss)
        jobs.append({
            "run_id": "MV0-n1-w%d-seed%s" % (wire, common["seed"]),
            "senders": 1,
            "nominal": int(nominal),
            "wire": wire,
            "rate": float(mv0["sender_rate_gbps"]),
            "queue": mvlib.queue_bytes(1, wire, mss),
        })
    measured = run_jobs(common, jobs)
    rows = []
    flow_rows = []
    link_rows = []
    for job, (record, flows, links) in zip(jobs, measured):
        row = blank_row(common, commit, "MV0", "calibration", "", 1,
                        job["nominal"], job["wire"], job["rate"], job["queue"], record)
        rows.append(row)
        for fl in flows:
            fl["run_id"] = record["run_id"]
            flow_rows.append(fl)
        for lk in links:
            lk["run_id"] = record["run_id"]
            link_rows.append(lk)

    usable = [r for r in rows if r["valid"] == "1" and int(r["sender_count"]) == 1]
    if len(usable) < 2:
        sys.stderr.write("MV0 did not produce enough valid one-sender points\n")
        write_csv(os.path.join(mvlib.RESULTS, "runs.csv"), RUN_FIELDS, rows)
        sys.exit(1)
    xs = [float(r["wire_message_bytes"]) for r in usable]
    ys = [float(r["sim_cct_us"]) * 1e-6 for r in usable]
    alpha_s, beta, r2 = mvlib.fit_alpha_beta(xs, ys)
    cal_payload = {
        "topology": common["topology"],
        "bottleneck_gbps": common["bottleneck_gbps"],
        "propagation_us": common["propagation_us"],
        "mss_bytes": mss,
        "seed": common["seed"],
        "wire_bytes": xs,
    }
    calibration_id = "mv0-" + mvlib.config_hash(cal_payload)
    print("  calibration_id %s" % calibration_id)
    print("  alpha_us %.6f  beta_ns_per_byte %.6f  R2 %.6f" % (alpha_s * 1e6, beta * 1e9, r2))

    points = []
    for r in rows:
        if r["experiment_id"] != "MV0":
            continue
        r["calibration_id"] = calibration_id
        apply_models(r, alpha_s, beta)
        if r["valid"] != "1":
            points.append({
                "calibration_id": calibration_id,
                "nominal_message_bytes": r["nominal_message_bytes"],
                "wire_message_bytes": r["wire_message_bytes"],
                "t_sim_us": r["sim_cct_us"],
                "t_fit_us": "",
                "residual_us": "",
                "valid": "0",
            })
            continue
        fit_us = float(r["T_M0_us"])
        sim_us = float(r["sim_cct_us"])
        points.append({
            "calibration_id": calibration_id,
            "nominal_message_bytes": r["nominal_message_bytes"],
            "wire_message_bytes": r["wire_message_bytes"],
            "t_sim_us": "%.16e" % sim_us,
            "t_fit_us": "%.16e" % fit_us,
            "residual_us": "%.16e" % (sim_us - fit_us),
            "valid": "1",
        })

    cal_row = {
        "calibration_id": calibration_id,
        "topology": common["topology"],
        "bottleneck_gbps": common["bottleneck_gbps"],
        "propagation_us": common["propagation_us"],
        "mss_bytes": mss,
        "seed": common["seed"],
        "sender_count": 1,
        "message_sizes_wire": " ".join(str(int(x)) for x in xs),
        "alpha_us": "%.16e" % (alpha_s * 1e6),
        "beta_ns_per_byte": "%.16e" % (beta * 1e9),
        "R2": "%.16e" % r2,
        "git_commit": commit,
        "source": "MV0_only",
    }

    mv1 = mvlib.load_json(os.path.join(mvlib.CONFIG_DIR, "mv1.json"))
    print("MV1 sender scaling")
    jobs = []
    for n in mv1["sender_counts"]:
        for nominal in mv1["nominal_message_bytes"]:
            wire = mvlib.wire_bytes(nominal, mss)
            jobs.append({
                "experiment": "MV1",
                "mode": mv1["mode"],
                "run_id": "MV1-n%d-w%d-seed%s" % (n, wire, common["seed"]),
                "senders": int(n),
                "nominal": int(nominal),
                "wire": wire,
                "rate": float(mv1["sender_rate_gbps"]),
                "queue": mvlib.queue_bytes(int(n), wire, mss),
            })
    # The N=1 sizes that MV0 already ran are not reused as fit data.
    # They are still executed here so MV1 is a complete matrix.
    measured = run_jobs(common, jobs)
    for job, (record, flows, links) in zip(jobs, measured):
        row = blank_row(common, commit, job["experiment"], job["mode"], calibration_id,
                        job["senders"], job["nominal"], job["wire"], job["rate"], job["queue"], record)
        apply_models(row, alpha_s, beta)
        rows.append(row)
        for fl in flows:
            fl["run_id"] = record["run_id"]
            flow_rows.append(fl)
        for lk in links:
            lk["run_id"] = record["run_id"]
            link_rows.append(lk)

    mv2 = mvlib.load_json(os.path.join(mvlib.CONFIG_DIR, "mv2.json"))
    print("MV2 sender count versus offered load")
    jobs = []
    for mode, spec in (("A", mv2["modes"]["A"]), ("B", mv2["modes"]["B"]), ("C", mv2["modes"]["C"])):
        for n in mv2["sender_counts"]:
            n = int(n)
            if spec["sender_rate"] == "line":
                rate = line
            elif spec["sender_rate"] == "line_over_n":
                rate = line / float(n)
            else:
                sys.stderr.write("unknown sender_rate %s\n" % spec["sender_rate"])
                sys.exit(1)
            if "nominal_bytes_per_sender" in spec:
                nominal = int(spec["nominal_bytes_per_sender"])
            else:
                total = int(spec["nominal_bytes_total"])
                if total % n != 0:
                    sys.stderr.write("mode C total is not divisible by N=%d\n" % n)
                    sys.exit(1)
                nominal = total // n
            wire = mvlib.wire_bytes(nominal, mss)
            jobs.append({
                "experiment": "MV2",
                "mode": mode,
                "run_id": "MV2-%s-n%d-w%d-seed%s" % (mode, n, wire, common["seed"]),
                "senders": n,
                "nominal": nominal,
                "wire": wire,
                "rate": rate,
                "queue": mvlib.queue_bytes(n, wire, mss),
            })
    measured = run_jobs(common, jobs)
    for job, (record, flows, links) in zip(jobs, measured):
        row = blank_row(common, commit, job["experiment"], job["mode"], calibration_id,
                        job["senders"], job["nominal"], job["wire"], job["rate"], job["queue"], record)
        apply_models(row, alpha_s, beta)
        rows.append(row)
        for fl in flows:
            fl["run_id"] = record["run_id"]
            flow_rows.append(fl)
        for lk in links:
            lk["run_id"] = record["run_id"]
            link_rows.append(lk)

    def kept(path, predicate):
        if not os.path.isfile(path):
            return []
        with open(path) as f:
            return [row for row in csv.DictReader(f) if predicate(row)]

    runs_path = os.path.join(mvlib.RESULTS, "runs.csv")
    flows_path = os.path.join(mvlib.RESULTS, "flows.csv")
    links_path = os.path.join(mvlib.RESULTS, "links.csv")
    rows.extend(kept(runs_path, lambda row: row.get("experiment_id") not in ("MV0", "MV1", "MV2")))
    flow_rows.extend(kept(flows_path, lambda row: row.get("run_id", "").startswith("MV3")))
    link_rows.extend(kept(links_path, lambda row: row.get("run_id", "").startswith("MV3")))
    write_csv(runs_path, RUN_FIELDS, rows)
    write_csv(flows_path, FLOW_FIELDS, flow_rows)
    write_csv(links_path, LINK_FIELDS, link_rows)
    write_csv(os.path.join(mvlib.RESULTS, "calibrations.csv"), list(cal_row.keys()), [cal_row])
    write_csv(os.path.join(mvlib.RESULTS, "calibration_points.csv"), POINT_FIELDS, points)
    print("Wrote %s" % os.path.join(mvlib.RESULTS, "runs.csv"))
    print("valid %d / %d" % (sum(r["valid"] == "1" for r in rows), len(rows)))


if __name__ == "__main__":
    main()
