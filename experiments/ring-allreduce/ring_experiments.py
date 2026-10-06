#!/usr/bin/env python3
"""Run the Experiment 1 and 2 controls and fit alpha, beta.

Experiment 1 must land near slowdown 1 against the fitted model before
Experiment 2 is started. Each htsim_ring invocation is one simulation.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.join(HERE, "htsim_ring")
OUTDIR = os.path.join(HERE, "ring_results")
MTU = 9000
N = 4
PACKETS_PER_CHUNK = 64
MESSAGE = N * PACKETS_PER_CHUNK * MTU
QUEUE = 8 * (MESSAGE // N)
CAL_PACKETS = [8, 16, 32, 64, 128, 256]
# Fitted-model slowdown on the uncontended ring. Wider than packet jitter,
# tight enough that a broken schedule fails the control.
EXP1_SLOWDOWN_LIMIT = 0.05


def run(args):
    cmd = [BINARY] + args + ["--mtu", str(MTU), "--queue-bytes", str(QUEUE), "--seed", "1"]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=180)
    record = None
    for line in proc.stdout.splitlines():
        if line.startswith("RECORD "):
            record = {}
            for tok in line.split()[1:]:
                key, value = tok.split("=", 1)
                record[key] = value
    if proc.returncode != 0 or record is None:
        sys.stderr.write("command failed: %s\n" % " ".join(cmd))
        sys.stderr.write(proc.stderr)
        sys.stderr.write(proc.stdout)
        sys.exit(1)
    return record, proc.stderr


def fit(points):
    # T = alpha + beta * bytes
    n = float(len(points))
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    var = sum((p[0] - mx) ** 2 for p in points)
    cov = sum((p[0] - mx) * (p[1] - my) for p in points)
    beta = cov / var
    alpha = my - beta * mx
    return alpha, beta


def collect_fit(experiment, pin):
    points = []
    rows = []
    for pkts in CAL_PACKETS:
        nbytes = pkts * MTU
        rec, err = run(["--mode", "p2p", "--experiment", str(experiment), "--pin", pin, "--n", str(N), "--bytes", str(nbytes)])
        if rec["valid"] != "1":
            sys.stderr.write(err)
            sys.stderr.write("calibration transfer was not loss-free\n")
            sys.exit(1)
        points.append((float(nbytes), float(rec["t_sim_s"])))
        rows.append(rec)
    alpha, beta = fit(points)
    return alpha, beta, rows


def write_csv(path, rows):
    keys = []
    for row in rows:
        for key in row:
            if key not in keys:
                keys.append(key)
    with open(path, "w") as f:
        f.write(",".join(keys) + "\n")
        for row in rows:
            f.write(",".join(row.get(k, "") for k in keys) + "\n")


def main():
    if not os.path.isfile(BINARY):
        sys.stderr.write("missing %s\n" % BINARY)
        sys.exit(1)
    os.makedirs(OUTDIR, exist_ok=True)

    print("Fitting experiment 1 on a same-leaf path")
    a1, b1, cal1 = collect_fit(1, "none")
    print("  alpha_fit_us %.6f  beta_fit_ns_per_byte %.6f" % (a1 * 1e6, b1 * 1e9))

    print("Experiment 1 collective, n=%d, s=%d bytes" % (N, MESSAGE))
    exp1, err1 = run([
        "--mode", "collective", "--experiment", "1", "--pin", "none", "--n", str(N),
        "--bytes", str(MESSAGE),
        "--alpha-fit", "%.16e" % a1, "--beta-fit", "%.16e" % b1,
    ])
    sys.stderr.write(err1)
    if exp1["valid"] != "1":
        print("Experiment 1 is invalid (loss or unfinished flows). Not starting experiment 2.")
        sys.exit(1)
    slow1 = float(exp1["slowdown_fit"])
    print("  T_sim_us %.3f  T_fit_us %.3f  slowdown_fit %.4f  L_max %s  H_max_us %.3f" % (
        float(exp1["t_sim_s"]) * 1e6, float(exp1["t_fit_s"]) * 1e6, slow1,
        exp1["l_max"], float(exp1["h_max_s"]) * 1e6))
    print("  alpha_theory_us %.3f  beta_theory_ns_per_byte %.6f  slowdown_theory %.4f" % (
        float(exp1["alpha_theory_s"]) * 1e6, float(exp1["beta_theory_s_per_byte"]) * 1e9,
        float(exp1["slowdown_theory"])))
    if abs(slow1 - 1.0) > EXP1_SLOWDOWN_LIMIT:
        print("Experiment 1 slowdown is outside %.0f%% of the fitted model. Not starting experiment 2." % (
            EXP1_SLOWDOWN_LIMIT * 100))
        write_csv(os.path.join(OUTDIR, "experiment1.csv"), cal1 + [exp1])
        sys.exit(1)

    print("Fitting experiment 2 on a cross-leaf path")
    a2, b2, cal2 = collect_fit(2, "spine0")
    print("  alpha_fit_us %.6f  beta_fit_ns_per_byte %.6f" % (a2 * 1e6, b2 * 1e9))

    exp2 = []
    for pin in ("spread", "stack"):
        print("Experiment 2 pin=%s" % pin)
        rec, err = run([
            "--mode", "collective", "--experiment", "2", "--pin", pin, "--n", str(N),
            "--bytes", str(MESSAGE),
            "--alpha-fit", "%.16e" % a2, "--beta-fit", "%.16e" % b2,
        ])
        sys.stderr.write(err)
        exp2.append(rec)
        if rec["valid"] != "1":
            print("Experiment 2 pin=%s is invalid. Stopping." % pin)
            write_csv(os.path.join(OUTDIR, "all_runs.csv"), cal1 + [exp1] + cal2 + exp2)
            sys.exit(1)
        print("  T_sim_us %.3f  T_fit_us %.3f  slowdown_fit %.4f  L_max %s  H_max_us %.3f  peak_bytes %s" % (
            float(rec["t_sim_s"]) * 1e6, float(rec["t_fit_s"]) * 1e6, float(rec["slowdown_fit"]),
            rec["l_max"], float(rec["h_max_s"]) * 1e6, rec["peak_queue_bytes"]))

    write_csv(os.path.join(OUTDIR, "all_runs.csv"), cal1 + [exp1] + cal2 + exp2)
    spread, stack = exp2
    print("Summary")
    print("  exp1 slowdown_fit %.4f (L_max %s)" % (slow1, exp1["l_max"]))
    print("  exp2 spread slowdown_fit %.4f (L_max %s, H_max_us %.3f)" % (
        float(spread["slowdown_fit"]), spread["l_max"], float(spread["h_max_s"]) * 1e6))
    print("  exp2 stack  slowdown_fit %.4f (L_max %s, H_max_us %.3f)" % (
        float(stack["slowdown_fit"]), stack["l_max"], float(stack["h_max_s"]) * 1e6))
    print("Wrote %s" % os.path.join(OUTDIR, "all_runs.csv"))


if __name__ == "__main__":
    main()
