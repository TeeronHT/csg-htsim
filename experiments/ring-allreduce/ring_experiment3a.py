#!/usr/bin/env python3
"""Experiment 3A: every directed 6-host ring on the three-leaf topology.

Host 0 is fixed at the start of each ring. Reverses are separate rows.
The static cost is the pinned forward propagation. Transport matches
Experiments 1 and 2.
"""

import itertools
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.join(HERE, "htsim_ring")
OUTDIR = os.path.join(HERE, "ring_results")
MTU = 9000
N = 6
PACKETS_PER_CHUNK = 64
MESSAGE = N * PACKETS_PER_CHUNK * MTU
QUEUE = 8 * (MESSAGE // N)
CAL_PACKETS = [8, 16, 32, 64, 128, 256]
WORKERS = 4


def parse_record(text):
    record = None
    for line in text.splitlines():
        if line.startswith("RECORD "):
            record = {}
            for tok in line.split()[1:]:
                key, value = tok.split("=", 1)
                record[key] = value
    return record


def run(args):
    cmd = [BINARY, "--experiment", "3", "--n", str(N), "--pin", "srcmod2",
           "--mtu", str(MTU), "--queue-bytes", str(QUEUE), "--seed", "1"] + args
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=180)
    record = parse_record(proc.stdout)
    if proc.returncode != 0 or record is None:
        sys.stderr.write("command failed: %s\n" % " ".join(cmd))
        sys.stderr.write(proc.stderr)
        sys.stderr.write(proc.stdout)
        sys.exit(1)
    return record


def fit(points):
    n = float(len(points))
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    var = sum((p[0] - mx) ** 2 for p in points)
    cov = sum((p[0] - mx) * (p[1] - my) for p in points)
    beta = cov / var
    alpha = my - beta * mx
    return alpha, beta


def collect_fit(src, dst):
    points = []
    for pkts in CAL_PACKETS:
        nbytes = pkts * MTU
        rec = run(["--mode", "p2p", "--src", str(src), "--dst", str(dst), "--bytes", str(nbytes)])
        if rec["valid"] != "1":
            sys.stderr.write("calibration %d->%d %d bytes was not loss-free\n" % (src, dst, nbytes))
            sys.exit(1)
        points.append((float(nbytes), float(rec["t_sim_s"])))
    return fit(points)


def rings():
    out = []
    for perm in itertools.permutations(range(1, N)):
        order = (0,) + perm
        out.append("-".join(str(h) for h in order))
    return out


def spearman(xs, ys):
    def ranks(values):
        order = sorted(range(len(values)), key=lambda i: values[i])
        out = [0.0] * len(values)
        i = 0
        while i < len(values):
            j = i
            while j + 1 < len(values) and values[order[j + 1]] == values[order[i]]:
                j += 1
            avg = (i + j) / 2.0 + 1.0
            for k in range(i, j + 1):
                out[order[k]] = avg
            i = j + 1
        return out

    rx, ry = ranks(xs), ranks(ys)
    n = float(len(xs))
    mx = sum(rx) / n
    my = sum(ry) / n
    num = sum((rx[i] - mx) * (ry[i] - my) for i in range(len(xs)))
    dx = sum((rx[i] - mx) ** 2 for i in range(len(xs))) ** 0.5
    dy = sum((ry[i] - my) ** 2 for i in range(len(ys))) ** 0.5
    if dx == 0 or dy == 0:
        return 0.0
    return num / (dx * dy)


def pct(values):
    ordered = sorted(values)
    return ordered[0], ordered[len(ordered) // 2], ordered[-1]


def write_csv(path, rows):
    keys = ["ring", "reverse", "shortcuts", "static_cost_ps", "l_max", "h_max_s",
            "t_fit_s", "t_sim_s", "slowdown_fit", "peak_queue_bytes",
            "drops", "strips", "bounces", "rtx", "nacks", "pauses", "sink_gaps",
            "flows_done", "flows_expected", "valid", "min_static_cost", "tsp_selected",
            "alpha_same_s", "beta_same_s_per_byte", "alpha_cross_s", "beta_cross_s_per_byte",
            "n", "bytes", "chunk", "seed", "pin"]
    with open(path, "w") as f:
        f.write(",".join(keys) + "\n")
        for row in rows:
            f.write(",".join(row.get(k, "") for k in keys) + "\n")


def summarize(rows, tsp_ring, a_same, b_same, a_cross, b_cross):
    lines = []
    def say(text):
        lines.append(text)
        print(text)

    valid = [r for r in rows if r["valid"] == "1"]
    say("Experiment 3A")
    say("  same-leaf fit  alpha_us %.6f  beta_ns_per_byte %.6f" % (a_same * 1e6, b_same * 1e9))
    say("  cross-leaf fit alpha_us %.6f  beta_ns_per_byte %.6f" % (a_cross * 1e6, b_cross * 1e9))
    say("  rings %d  valid %d" % (len(rows), len(valid)))
    if len(valid) != len(rows):
        say("  invalid rings are excluded from the ranking below")
    if not valid:
        return "\n".join(lines) + "\n"

    for r in valid:
        r["_c"] = int(r["static_cost_ps"])
        r["_t"] = float(r["t_sim_s"])
        r["_l"] = int(r["l_max"])
        r["_h"] = float(r["h_max_s"])

    min_c = min(r["_c"] for r in valid)
    min_t = min(r["_t"] for r in valid)
    static_set = [r for r in valid if r["_c"] == min_c]
    runtime_set = [r for r in valid if r["_t"] == min_t]
    near = [r for r in valid if r["_t"] <= min_t * 1.01]
    tsp_rows = [r for r in valid if r["ring"] == tsp_ring]

    say("  minimum static cost %.3f us, %d rings" % (min_c / 1e6, len(static_set)))
    say("    L_max values %s" % sorted(set(r["_l"] for r in static_set)))
    say("    T_sim us %.3f .. %.3f" % (
        min(r["_t"] for r in static_set) * 1e6, max(r["_t"] for r in static_set) * 1e6))
    say("  Held-Karp selected %s" % tsp_ring)
    if tsp_rows:
        tr = tsp_rows[0]
        say("    C_us %.3f  T_sim_us %.3f  L_max %d  in minimum-cost set %s  strict runtime minimum %s" % (
            tr["_c"] / 1e6, tr["_t"] * 1e6, tr["_l"],
            "yes" if tr["_c"] == min_c else "no",
            "yes" if tr["_t"] == min_t else "no"))
    say("  strict minimum runtime %.3f us, %d rings" % (min_t * 1e6, len(runtime_set)))
    for r in runtime_set:
        say("    %s  C_us %.3f  L_max %d" % (r["ring"], r["_c"] / 1e6, r["_l"]))
    say("  rings within 1%% of the minimum runtime: %d" % len(near))
    same = any(r["ring"] == tsp_ring for r in runtime_set)
    say("  Held-Karp ring is a strict runtime minimum: %s" % ("yes" if same else "no"))

    say("  Spearman rank correlation with T_sim")
    say("    static cost %.4f" % spearman([r["_c"] for r in valid], [r["_t"] for r in valid]))
    say("    L_max       %.4f" % spearman([r["_l"] for r in valid], [r["_t"] for r in valid]))
    say("    H_max       %.4f" % spearman([r["_h"] for r in valid], [r["_t"] for r in valid]))

    say("  runtime by static-cost bucket")
    buckets = {}
    for r in valid:
        buckets.setdefault(r["_c"], []).append(r)
    for cost in sorted(buckets):
        group = buckets[cost]
        lo, mid, hi = pct([r["_t"] for r in group])
        by_l = {}
        for r in group:
            by_l.setdefault(r["_l"], []).append(r["_t"])
        parts = []
        for ell in sorted(by_l):
            a, b, c = pct(by_l[ell])
            parts.append("L=%d n=%d T_us %.3f..%.3f" % (ell, len(by_l[ell]), a * 1e6, c * 1e6))
        say("    C_us %.3f  n=%d  T_us %.3f .. %.3f  median %.3f  %s" % (
            cost / 1e6, len(group), lo * 1e6, hi * 1e6, mid * 1e6, "; ".join(parts)))

    say("  runtime by L_max")
    by_l = {}
    for r in valid:
        by_l.setdefault(r["_l"], []).append(r["_t"])
    for ell in sorted(by_l):
        lo, mid, hi = pct(by_l[ell])
        say("    L_max %d  n=%d  T_us %.3f .. %.3f  median %.3f" % (
            ell, len(by_l[ell]), lo * 1e6, hi * 1e6, mid * 1e6))

    inversions = 0
    worst = None
    for i in range(len(valid)):
        for j in range(len(valid)):
            if valid[i]["_c"] < valid[j]["_c"] and valid[i]["_t"] > valid[j]["_t"]:
                inversions += 1
                gap = valid[i]["_t"] - valid[j]["_t"]
                if worst is None or gap > worst[0]:
                    worst = (gap, valid[i], valid[j])
    say("  pairs with C(A)<C(B) but T_sim(A)>T_sim(B): %d" % inversions)
    if worst:
        gap, a, b = worst
        say("    largest gap %.3f us: %s (C %.3f us, T %.3f us, L %d) slower than %s (C %.3f us, T %.3f us, L %d)" % (
            gap * 1e6, a["ring"], a["_c"] / 1e6, a["_t"] * 1e6, a["_l"],
            b["ring"], b["_c"] / 1e6, b["_t"] * 1e6, b["_l"]))

    say("  reverse pairs with equal static cost")
    indexed = dict((r["ring"], r) for r in valid)
    seen = set()
    diffs = []
    for r in valid:
        other = indexed.get(r["reverse"])
        if other is None:
            continue
        key = tuple(sorted((r["ring"], other["ring"])))
        if key in seen:
            continue
        seen.add(key)
        if r["_c"] != other["_c"]:
            say("    cost mismatch %s %d vs %s %d" % (r["ring"], r["_c"], other["ring"], other["_c"]))
            continue
        base = min(r["_t"], other["_t"])
        rel = abs(r["_t"] - other["_t"]) / base
        diffs.append((rel, r, other))
    diffs.sort(key=lambda item: item[0], reverse=True)
    meaningful = [item for item in diffs if item[0] > 0.01]
    say("    pairs %d  relative gap above 1%%: %d" % (len(diffs), len(meaningful)))
    if diffs:
        rel, a, b = diffs[0]
        say("    largest gap %.2f%%: %s T_us %.3f L %d vs %s T_us %.3f L %d" % (
            rel * 100, a["ring"], a["_t"] * 1e6, a["_l"], b["ring"], b["_t"] * 1e6, b["_l"]))
    return "\n".join(lines) + "\n"


def main():
    if not os.path.isfile(BINARY):
        sys.stderr.write("missing %s\n" % BINARY)
        sys.exit(1)
    os.makedirs(OUTDIR, exist_ok=True)

    print("Fitting same-leaf 0->1")
    a_same, b_same = collect_fit(0, 1)
    print("  alpha_us %.6f  beta_ns_per_byte %.6f" % (a_same * 1e6, b_same * 1e9))
    print("Fitting cross-leaf 0->2")
    a_cross, b_cross = collect_fit(0, 2)
    print("  alpha_us %.6f  beta_ns_per_byte %.6f" % (a_cross * 1e6, b_cross * 1e9))

    tsp = run(["--mode", "tsp", "--bytes", str(MESSAGE)])
    tsp_ring = tsp["ring"]
    print("Held-Karp ring %s  static_cost_ps %s" % (tsp_ring, tsp["static_cost_ps"]))

    fit_args = [
        "--alpha-same", "%.16e" % a_same, "--beta-same", "%.16e" % b_same,
        "--alpha-cross", "%.16e" % a_cross, "--beta-cross", "%.16e" % b_cross,
    ]
    order = rings()
    print("Running %d directed rings" % len(order))

    def one(ring):
        return run(["--mode", "collective", "--bytes", str(MESSAGE), "--ring", ring] + fit_args)

    done = 0
    results = [None] * len(order)
    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        futures = []
        for idx, ring in enumerate(order):
            futures.append((idx, pool.submit(one, ring)))
        for idx, fut in futures:
            results[idx] = fut.result()
            done += 1
            if done % 10 == 0 or done == len(order):
                print("  finished %d/%d" % (done, len(order)))

    for rec in results:
        rec["min_static_cost"] = "0"
        rec["tsp_selected"] = "1" if rec["ring"] == tsp_ring else "0"
    min_c = min(int(rec["static_cost_ps"]) for rec in results)
    for rec in results:
        if int(rec["static_cost_ps"]) == min_c:
            rec["min_static_cost"] = "1"

    csv_path = os.path.join(OUTDIR, "experiment3a.csv")
    write_csv(csv_path, results)
    text = summarize(results, tsp_ring, a_same, b_same, a_cross, b_cross)
    summary_path = os.path.join(OUTDIR, "experiment3a_summary.txt")
    with open(summary_path, "w") as f:
        f.write(text)
    print("Wrote %s" % csv_path)
    print("Wrote %s" % summary_path)


if __name__ == "__main__":
    main()
