#!/usr/bin/env python3
"""Simple plots for MV0, MV1, and MV2. Invalid rows are omitted from the lines."""

import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvlib

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load(name):
    with open(os.path.join(mvlib.RESULTS, name)) as f:
        return list(csv.DictReader(f))


def valid(rows, experiment):
    out = []
    for row in rows:
        if row["experiment_id"] == experiment and row["valid"] == "1":
            out.append(row)
    return out


def save(fig, name):
    path = os.path.join(mvlib.RESULTS, "plots", name)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    print("wrote %s" % path)


def main():
    os.makedirs(os.path.join(mvlib.RESULTS, "plots"), exist_ok=True)
    runs = load("runs.csv")
    points = load("calibration_points.csv")
    good_points = [p for p in points if p["valid"] == "1"]

    fig, ax = plt.subplots()
    wire = [float(p["wire_message_bytes"]) for p in good_points]
    sim = [float(p["t_sim_us"]) for p in good_points]
    fit = [float(p["t_fit_us"]) for p in good_points]
    order = sorted(range(len(wire)), key=lambda i: wire[i])
    ax.plot([wire[i] for i in order], [sim[i] for i in order], "o", label="simulated FCT")
    ax.plot([wire[i] for i in order], [fit[i] for i in order], "-", label="alpha + beta * bytes")
    ax.set_xlabel("wire bytes")
    ax.set_ylabel("FCT (us)")
    ax.set_title("MV0 isolated calibration")
    ax.legend()
    save(fig, "mv0_fit.png")

    fig, ax = plt.subplots()
    ax.axhline(0, color="black", linewidth=0.8)
    ax.plot([wire[i] for i in order], [sim[i] - fit[i] for i in order], "o-")
    ax.set_xlabel("wire bytes")
    ax.set_ylabel("residual (us)")
    ax.set_title("MV0 fit residual")
    save(fig, "mv0_residuals.png")

    mv1 = valid(runs, "MV1")
    def size_label(nominal):
        if nominal % (1024 * 1024) == 0:
            return "%d MiB nominal" % (nominal // (1024 * 1024))
        return "%d KiB nominal" % (nominal // 1024)

    sizes = sorted(set(int(r["nominal_message_bytes"]) for r in mv1))
    for column, fname, title in (
        ("ratio_sim_M0", "mv1_ratio_m0.png", "MV1 sim / isolated alpha-beta"),
        ("ratio_sim_M1_capacity", "mv1_ratio_m1_capacity.png", "MV1 sim / capacity fair share"),
        ("ratio_sim_M1_beta", "mv1_ratio_m1_beta.png", "MV1 sim / scaled fitted beta"),
    ):
        fig, ax = plt.subplots()
        for nominal in sizes:
            group = [r for r in mv1 if int(r["nominal_message_bytes"]) == nominal]
            group.sort(key=lambda r: int(r["sender_count"]))
            ax.plot([int(r["sender_count"]) for r in group],
                    [float(r[column]) for r in group],
                    "o-", label=size_label(nominal))
        ax.set_xlabel("senders N")
        ax.set_ylabel("ratio")
        ax.set_title(title)
        ax.legend()
        save(fig, fname)

    fig, ax = plt.subplots()
    for nominal in sizes:
        group = [r for r in mv1 if int(r["nominal_message_bytes"]) == nominal]
        group.sort(key=lambda r: int(r["sender_count"]))
        ax.plot([int(r["sender_count"]) for r in group],
                [float(r["peak_queue_bytes"]) for r in group],
                "o-", label=size_label(nominal))
    ax.set_xlabel("senders N")
    ax.set_ylabel("peak bottleneck queue (bytes)")
    ax.set_title("MV1 peak queue")
    ax.legend()
    save(fig, "mv1_peak_queue.png")

    mv2 = valid(runs, "MV2")
    fig, ax = plt.subplots()
    for mode in ("A", "B", "C"):
        group = [r for r in mv2 if r["mode"] == mode]
        group.sort(key=lambda r: int(r["sender_count"]))
        ax.plot([int(r["sender_count"]) for r in group],
                [float(r["sim_cct_us"]) for r in group],
                "o-", label="mode " + mode)
    ax.set_xlabel("senders N")
    ax.set_ylabel("sim CCT (us)")
    ax.set_title("MV2 completion time")
    ax.legend()
    save(fig, "mv2_cct.png")

    fig, ax = plt.subplots()
    for mode in ("A", "B", "C"):
        group = [r for r in mv2 if r["mode"] == mode]
        group.sort(key=lambda r: int(r["sender_count"]))
        ax.plot([int(r["sender_count"]) for r in group],
                [float(r["aggregate_achieved_rate_gbps"]) for r in group],
                "o-", label="mode " + mode)
    ax.set_xlabel("senders N")
    ax.set_ylabel("aggregate achieved rate (Gbps)")
    ax.set_title("MV2 aggregate throughput")
    ax.legend()
    save(fig, "mv2_throughput.png")

    fig, ax = plt.subplots()
    for mode in ("A", "B", "C"):
        group = [r for r in mv2 if r["mode"] == mode]
        group.sort(key=lambda r: int(r["sender_count"]))
        ax.plot([int(r["sender_count"]) for r in group],
                [float(r["peak_queue_bytes"]) for r in group],
                "o-", label="mode " + mode)
    ax.set_xlabel("senders N")
    ax.set_ylabel("peak bottleneck queue (bytes)")
    ax.set_title("MV2 peak queue")
    ax.legend()
    save(fig, "mv2_peak_queue.png")

    mv3 = valid(runs, "MV3")
    if not mv3:
        return
    flows = load("flows.csv")
    sizes = sorted(set(int(r["nominal_message_bytes"]) for r in mv3))
    for column, fname, title in (
        ("ratio_sim_M1_static", "mv3_ratio_m1_static.png", "MV3 sim makespan / static C/8"),
        ("ratio_sim_M2_active", "mv3_ratio_m2_active.png", "MV3 sim makespan / active-set fluid"),
        ("ratio_sim_M2_packet", "mv3_ratio_m2_packet.png", "MV3 sim makespan / packet-aware fluid"),
    ):
        fig, ax = plt.subplots()
        for nominal in sizes:
            group = [r for r in mv3 if int(r["nominal_message_bytes"]) == nominal]
            group.sort(key=lambda r: float(r["start_spread_over_Tiso"]))
            ax.plot([float(r["start_spread_over_Tiso"]) for r in group],
                    [float(r[column]) for r in group],
                    "o-", label=size_label(nominal))
        ax.set_xlabel("start spread / T_iso")
        ax.set_ylabel("makespan ratio")
        ax.set_title(title)
        ax.legend()
        save(fig, fname)

    fig, ax = plt.subplots()
    for nominal in sizes:
        group = [r for r in mv3 if int(r["nominal_message_bytes"]) == nominal]
        group.sort(key=lambda r: float(r["start_spread_over_Tiso"]))
        ax.plot([float(r["start_spread_over_Tiso"]) for r in group],
                [float(r["peak_queue_bytes"]) for r in group],
                "o-", label=size_label(nominal))
    ax.set_xlabel("start spread / T_iso")
    ax.set_ylabel("peak bottleneck queue (bytes)")
    ax.set_title("MV3 peak queue")
    ax.legend()
    save(fig, "mv3_peak_queue.png")

    fig, ax = plt.subplots()
    for nominal in sizes:
        group = [r for r in mv3 if int(r["nominal_message_bytes"]) == nominal]
        group.sort(key=lambda r: float(r["start_spread_over_Tiso"]))
        ax.plot([float(r["start_spread_over_Tiso"]) for r in group],
                [float(r["queue_integral_byte_us"]) for r in group],
                "o-", label=size_label(nominal))
    ax.set_xlabel("start spread / T_iso")
    ax.set_ylabel("queue integral (byte-us)")
    ax.set_title("MV3 queue integral")
    ax.legend()
    save(fig, "mv3_queue_integral.png")

    fig, ax = plt.subplots()
    for nominal in sizes:
        group = [r for r in mv3 if int(r["nominal_message_bytes"]) == nominal]
        group.sort(key=lambda r: float(r["start_spread_over_Tiso"]))
        ax.plot([float(r["start_spread_over_Tiso"]) for r in group],
                [float(r["contention_tail_us"]) for r in group],
                "o-", label=size_label(nominal))
    ax.set_xlabel("start spread / T_iso")
    ax.set_ylabel("contention tail (us)")
    ax.set_title("MV3 contention tail")
    ax.legend()
    save(fig, "mv3_contention_tail.png")

    fig, ax = plt.subplots()
    for nominal in sizes:
        group = [r for r in mv3 if int(r["nominal_message_bytes"]) == nominal]
        group.sort(key=lambda r: float(r["start_spread_over_Tiso"]))
        ax.plot([float(r["start_spread_over_Tiso"]) for r in group],
                [int(r["max_active_flows"]) for r in group],
                "o-", label=size_label(nominal))
    ax.set_xlabel("start spread / T_iso")
    ax.set_ylabel("max active flows")
    ax.set_title("MV3 maximum concurrency")
    ax.legend()
    save(fig, "mv3_max_active.png")

    fig, ax = plt.subplots()
    for nominal in sizes:
        group = [r for r in mv3 if int(r["nominal_message_bytes"]) == nominal]
        group.sort(key=lambda r: float(r["start_spread_over_Tiso"]))
        ax.plot([float(r["start_spread_over_Tiso"]) for r in group],
                [float(r["time_weighted_mean_active_flows"]) for r in group],
                "o-", label=size_label(nominal))
    ax.set_xlabel("start spread / T_iso")
    ax.set_ylabel("mean active flows")
    ax.set_title("MV3 mean concurrency")
    ax.legend()
    save(fig, "mv3_mean_active.png")

    wanted = {0.0, 0.5, 2.0, 4.0, 7.0, 10.0}
    fig, ax = plt.subplots()
    for row in mv3:
        if int(row["nominal_message_bytes"]) != 8388608:
            continue
        fraction = float(row["start_spread_over_Tiso"])
        if not any(abs(fraction - item) < 1e-9 for item in wanted):
            continue
        group = [fl for fl in flows if fl["run_id"] == row["run_id"]]
        group.sort(key=lambda fl: int(fl["flow_id"]))
        ax.plot([int(fl["flow_id"]) for fl in group],
                [float(fl["fct_us"]) for fl in group],
                "o-", label="S/T=%.2f" % fraction)
    ax.set_xlabel("flow index (start order)")
    ax.set_ylabel("FCT (us)")
    ax.set_title("MV3 per-flow FCT, 8 MiB")
    ax.legend()
    save(fig, "mv3_fct_by_start.png")

    fig, ax = plt.subplots()
    for fraction, label in ((0.0, "S/T=0"), (2.0, "S/T=2"), (7.0, "S/T=7"), (10.0, "S/T=10")):
        match = None
        for row in mv3:
            if int(row["nominal_message_bytes"]) != 8388608:
                continue
            if abs(float(row["start_spread_over_Tiso"]) - fraction) < 1e-9:
                match = row
        if match is None:
            continue
        path = os.path.join(mvlib.RESULTS, "traces", match["run_id"] + "_active_flows.csv")
        times = []
        counts = []
        with open(path) as f:
            next(f)
            for line in f:
                time, count = line.split(",")
                times.append(float(time))
                counts.append(int(count))
        ax.step(times, counts, where="post", label=label)
    ax.set_xlabel("time (us)")
    ax.set_ylabel("active flows")
    ax.set_title("MV3 active flows, 8 MiB")
    ax.legend()
    save(fig, "mv3_active_flows.png")

    mv4 = valid(runs, "MV4")
    if not mv4:
        return
    order = ["NONE", "CONSTANT", "BURSTY_FAST", "BURSTY_MEDIUM", "BURSTY_SLOW", "DISJOINT_CONSTANT"]

    def mv4_key(row):
        phase = row["background_phase_fraction"]
        return (order.index(row["background_pattern"]), float(phase) if phase else -1.0)

    def mv4_label(row):
        phase = row["background_phase_fraction"]
        name = row["background_pattern"]
        if not phase:
            return name
        return "%s p=%g" % (name, float(phase))

    mv4.sort(key=mv4_key)
    labels = [mv4_label(r) for r in mv4]
    xpos = list(range(len(mv4)))

    fig, ax = plt.subplots(figsize=(10, 4))
    ax.bar(xpos, [float(r["foreground_sim_fct_us"]) for r in mv4])
    ax.set_xticks(xpos)
    ax.set_xticklabels(labels, rotation=30, ha="right")
    ax.set_ylabel("foreground FCT (us)")
    ax.set_title("MV4 foreground completion")
    save(fig, "mv4_foreground_fct.png")

    fig, ax = plt.subplots(figsize=(10, 4))
    ax.plot(xpos, [float(r["ratio_sim_M1_average"]) for r in mv4], "o-", label="sim / M1 average")
    ax.plot(xpos, [float(r["ratio_sim_M2_timeline_packet"]) for r in mv4], "s-", label="sim / M2 packet")
    ax.axhline(1.0, color="gray", linewidth=0.8)
    ax.set_xticks(xpos)
    ax.set_xticklabels(labels, rotation=30, ha="right")
    ax.set_ylabel("foreground FCT ratio")
    ax.set_title("MV4 average model vs timeline model")
    ax.legend()
    save(fig, "mv4_model_ratios.png")


if __name__ == "__main__":
    main()
