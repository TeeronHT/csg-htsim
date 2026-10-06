#!/usr/bin/env python3
"""Experiment 3B: every directed 6-host ring on the two-site fiber.

Same plan space, message, queue, seed, and RoCE settings as Experiment 3A.
The only change is the topology: same-site edges are 4 us / 2 hops, and
inter-site edges are 3 us / 3 hops over one fiber in each direction.
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
    cmd = [BINARY, "--experiment", "4", "--n", str(N), "--pin", "shortest",
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


def collect_fit(src, dst, label):
    points = []
    print("Fitting %s %d->%d" % (label, src, dst))
    for pkts in CAL_PACKETS:
        nbytes = pkts * MTU
        rec = run(["--mode", "p2p", "--src", str(src), "--dst", str(dst), "--bytes", str(nbytes)])
        if rec["valid"] != "1":
            sys.stderr.write("calibration %d->%d %d bytes was not loss-free\n" % (src, dst, nbytes))
            sys.exit(1)
        points.append((float(nbytes), float(rec["t_sim_s"])))
        print("  %d bytes  t_sim_us %.3f" % (nbytes, float(rec["t_sim_s"]) * 1e6))
    alpha, beta = fit(points)
    print("  alpha_us %.6f  beta_ns_per_byte %.6f" % (alpha * 1e6, beta * 1e9))
    return alpha, beta, points


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
    keys = ["ring", "reverse", "inter_site", "static_cost_ps", "l_max", "h_max_s",
            "t_fit_s", "t_sim_s", "slowdown_fit", "peak_queue_bytes", "peak_queue",
            "fiber_peak_bytes",
            "drops", "strips", "bounces", "rtx", "nacks", "pauses", "sink_gaps",
            "flows_done", "flows_expected", "valid", "min_static_cost", "tsp_selected",
            "alpha_same_s", "beta_same_s_per_byte", "alpha_cross_s", "beta_cross_s_per_byte",
            "n", "bytes", "chunk", "seed", "pin"]
    with open(path, "w") as f:
        f.write(",".join(keys) + "\n")
        for row in rows:
            f.write(",".join(row.get(k, "") for k in keys) + "\n")


def class_label(r):
    return "C=%.0fus L_max=%d" % (r["_c"] / 1e6, r["_l"])


def summarize(rows, tsp_ring, a_same, b_same, a_cross, b_cross):
    lines = []

    def say(text):
        lines.append(text)
        print(text)

    valid = [r for r in rows if r["valid"] == "1"]
    say("Experiment 3B")
    say("  same-site fit  0->1  alpha_us %.6f  beta_ns_per_byte %.6f" % (a_same * 1e6, b_same * 1e9))
    say("  inter-site fit 0->3  alpha_us %.6f  beta_ns_per_byte %.6f" % (a_cross * 1e6, b_cross * 1e9))
    chunk = MESSAGE // N
    step_same = a_same + chunk * b_same
    step_cross = a_cross + chunk * b_cross
    if step_same > step_cross:
        slower = "same-site"
    elif step_cross > step_same:
        slower = "inter-site"
    else:
        slower = "tied"
    say("  isolated chunk step  same-site %.3f us  inter-site %.3f us  slower class: %s" % (
        step_same * 1e6, step_cross * 1e6, slower))
    say("  pairwise prediction uses the slower class present in each ring")
    say("  rings %d  valid %d" % (len(rows), len(valid)))

    counters = ["drops", "strips", "bounces", "rtx", "nacks", "pauses", "sink_gaps"]
    bad = []
    for r in rows:
        if r["valid"] != "1" or any(r.get(k, "0") != "0" for k in counters):
            bad.append(r["ring"])
    say("9. every row valid with zero loss/recovery/control effects: %s" % (
        "yes" if not bad and len(valid) == len(rows) else "no"))
    if bad:
        say("   failing rings: %s" % ", ".join(bad))
    if not valid:
        return "\n".join(lines) + "\n"

    for r in valid:
        r["_c"] = int(r["static_cost_ps"])
        r["_t"] = float(r["t_sim_s"])
        r["_l"] = int(r["l_max"])
        r["_h"] = float(r["h_max_s"])
        r["_fit"] = float(r["t_fit_s"])
        r["_slow"] = float(r["slowdown_fit"])
        r["_fiber"] = int(r["fiber_peak_bytes"])
        r["_peak"] = int(r["peak_queue_bytes"])
        r["_cross"] = int(r["inter_site"])

    min_c = min(r["_c"] for r in valid)
    min_t = min(r["_t"] for r in valid)
    static_set = [r for r in valid if r["_c"] == min_c]
    runtime_set = [r for r in valid if r["_t"] == min_t]
    static_ids = set(r["ring"] for r in static_set)
    runtime_ids = set(r["ring"] for r in runtime_set)
    overlap = static_ids & runtime_ids
    uncontended = [r for r in valid if r["_l"] == 1]
    fast_u = min(r["_t"] for r in uncontended)
    fast_u_set = [r for r in uncontended if r["_t"] == fast_u]
    tsp_rows = [r for r in valid if r["ring"] == tsp_ring]

    say("")
    say("1. minimum static cost %.3f us, %d rings, all L_max %s" % (
        min_c / 1e6, len(static_set), sorted(set(r["_l"] for r in static_set))))
    say("   T_sim us %.3f .. %.3f" % (
        min(r["_t"] for r in static_set) * 1e6, max(r["_t"] for r in static_set) * 1e6))
    for r in static_set:
        say("   %s  inter_site %d  L_max %d  T_sim_us %.3f" % (
            r["ring"], r["_cross"], r["_l"], r["_t"] * 1e6))

    say("2. minimum simulated completion time %.3f us, %d rings" % (min_t * 1e6, len(runtime_set)))
    for r in runtime_set:
        say("   %s  C_us %.3f  inter_site %d  L_max %d" % (
            r["ring"], r["_c"] / 1e6, r["_cross"], r["_l"]))

    say("3. minimum-static-cost set overlaps the minimum-runtime set: %s" % (
        "yes" if overlap else "no"))
    if overlap:
        say("   shared rings: %s" % ", ".join(sorted(overlap)))

    say("4. Held-Karp selected %s" % tsp_ring)
    if tsp_rows:
        tr = tsp_rows[0]
        say("   C_us %.3f  inter_site %d  L_max %d  H_max_us %.3f  T_sim_us %.3f  T_fit_us %.3f  slowdown %.4f" % (
            tr["_c"] / 1e6, tr["_cross"], tr["_l"], tr["_h"] * 1e6,
            tr["_t"] * 1e6, tr["_fit"] * 1e6, tr["_slow"]))
        say("   in minimum-cost set: %s" % ("yes" if tr["_c"] == min_c else "no"))

    say("5. fastest uncontended ring (L_max=1), T_sim_us %.3f, %d rings" % (
        fast_u * 1e6, len(fast_u_set)))
    for r in fast_u_set:
        say("   %s  C_us %.3f  inter_site %d" % (r["ring"], r["_c"] / 1e6, r["_cross"]))

    if tsp_rows:
        tr = tsp_rows[0]
        penalty = tr["_t"] - fast_u
        say("6. penalty of the Held-Karp ring versus the fastest uncontended ring")
        say("   absolute %.3f us  ratio %.4f  (T_hk / T_uncontended)" % (
            penalty * 1e6, tr["_t"] / fast_u))
        say("   versus the global minimum runtime: %.3f us  ratio %.4f" % (
            (tr["_t"] - min_t) * 1e6, tr["_t"] / min_t))

    say("7. Spearman rank correlation with T_sim")
    say("   static cost %.4f" % spearman([r["_c"] for r in valid], [r["_t"] for r in valid]))
    say("   L_max       %.4f" % spearman([r["_l"] for r in valid], [r["_t"] for r in valid]))
    say("   H_max       %.4f" % spearman([r["_h"] for r in valid], [r["_t"] for r in valid]))

    say("8. peak fiber queue occupancy by contention class")
    by_l = {}
    for r in valid:
        by_l.setdefault(r["_l"], []).append(r)
    for ell in sorted(by_l):
        group = by_l[ell]
        lo, mid, hi = pct([r["_fiber"] for r in group])
        say("   L_max %d  n=%d  fiber_peak_bytes %d .. %d  median %d" % (
            ell, len(group), lo, hi, mid))

    say("")
    say("structural classes")
    groups = {}
    for r in valid:
        groups.setdefault((r["_c"], r["_l"]), []).append(r)
    for key in sorted(groups):
        group = groups[key]
        lo, mid, hi = pct([r["_t"] for r in group])
        flo, _, fhi = pct([r["_fiber"] for r in group])
        slo, _, shi = pct([r["_slow"] for r in group])
        say("  C_us %.0f  L_max %d  n=%d  inter_site %s" % (
            key[0] / 1e6, key[1], len(group), sorted(set(r["_cross"] for r in group))))
        say("    T_sim_us %.3f .. %.3f  median %.3f" % (lo * 1e6, hi * 1e6, mid * 1e6))
        say("    T_fit_us %.3f  slowdown %.4f .. %.4f" % (
            group[0]["_fit"] * 1e6, slo, shi))
        say("    fiber_peak_bytes %d .. %d  peak_queue_bytes %d .. %d" % (
            flo, fhi, min(r["_peak"] for r in group), max(r["_peak"] for r in group)))

    return "\n".join(lines) + "\n"


def main():
    if not os.path.isfile(BINARY):
        sys.stderr.write("missing %s\n" % BINARY)
        sys.exit(1)
    os.makedirs(OUTDIR, exist_ok=True)

    a_same, b_same, _ = collect_fit(0, 1, "same-site")
    a_cross, b_cross, _ = collect_fit(0, 3, "inter-site")

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

    csv_path = os.path.join(OUTDIR, "experiment3b.csv")
    write_csv(csv_path, results)
    text = summarize(results, tsp_ring, a_same, b_same, a_cross, b_cross)
    summary_path = os.path.join(OUTDIR, "experiment3b_summary.txt")
    with open(summary_path, "w") as f:
        f.write(text)
    print("Wrote %s" % csv_path)
    print("Wrote %s" % summary_path)


if __name__ == "__main__":
    main()
