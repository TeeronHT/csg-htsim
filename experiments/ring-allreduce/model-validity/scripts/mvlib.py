"""Shared config, MSS rounding, and model formulas for the model-validity suite."""

import hashlib
import json
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SUITE = os.path.abspath(os.path.join(HERE, ".."))
ROOT = os.path.abspath(os.path.join(SUITE, ".."))
BINARY = os.path.join(SUITE, "htsim_mv")
CONFIG_DIR = os.path.join(SUITE, "configs")
RESULTS = os.path.join(SUITE, "results")


def load_json(path):
    with open(path) as f:
        return json.load(f)


def common_config():
    return load_json(os.path.join(CONFIG_DIR, "common.json"))


def wire_bytes(nominal, mss):
    nominal = int(nominal)
    mss = int(mss)
    if nominal <= 0:
        raise ValueError("nominal size must be positive")
    return ((nominal + mss - 1) // mss) * mss


def queue_bytes(senders, per_sender_wire, mss):
    """Hold the incast backlog plus one extra message."""
    per_sender_wire = int(per_sender_wire)
    margin = max(per_sender_wire, 64 * int(mss))
    return max(0, int(senders) - 1) * per_sender_wire + margin


def config_hash(payload):
    blob = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()[:16]


def git_commit():
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=os.path.abspath(os.path.join(ROOT, "..", "..")),
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
        )
        return out.strip()
    except (subprocess.CalledProcessError, OSError):
        return "unknown"


def fit_alpha_beta(xs, ys):
    n = float(len(xs))
    mx = sum(xs) / n
    my = sum(ys) / n
    var = sum((x - mx) ** 2 for x in xs)
    cov = sum((xs[i] - mx) * (ys[i] - my) for i in range(len(xs)))
    if var == 0:
        raise ValueError("calibration sizes do not vary")
    beta = cov / var
    alpha = my - beta * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((ys[i] - (alpha + beta * xs[i])) ** 2 for i in range(len(xs)))
    r2 = 1.0 if ss_tot == 0 else 1.0 - ss_res / ss_tot
    return alpha, beta, r2


# Queue service size of one paced RoCE data packet on this build: 9000-byte
# payload plus the 64-byte header the queue accounts for.
PACKET_ON_CORE_BYTES = 9064


def predictions(alpha_s, beta_s_per_byte, wire, senders, bottleneck_gbps):
    """Return seconds. m is wire payload bytes. Capacity share uses the link rate."""
    m = float(wire)
    n = float(senders)
    capacity_Bps = float(bottleneck_gbps) * 1e9 / 8.0
    share = capacity_Bps / n
    t0 = alpha_s + beta_s_per_byte * m
    t_cap = alpha_s + m / share
    t_beta = alpha_s + n * beta_s_per_byte * m
    return t0, t_cap, t_beta


def fluid_finish_times(start_s, remaining_bytes, capacity_Bps, alpha_s):
    """Event-driven equal-share predictor.

    A flow does not consume the core until `start + alpha`. Alpha is the
    isolated fit intercept, a fixed startup delay. After that, every eligible
    unfinished flow receives `capacity_Bps / k`. The predictor never reads a
    simulated completion time.
    """
    n = len(start_s)
    eligible = [float(start_s[i]) + float(alpha_s) for i in range(n)]
    rem = [float(b) for b in remaining_bytes]
    finish = [None] * n
    t = 0.0
    guard = 0
    while any(item is None for item in finish):
        guard += 1
        if guard > 100000:
            raise RuntimeError("fluid predictor did not finish")
        active = [i for i in range(n) if finish[i] is None and eligible[i] <= t + 1e-15]
        candidates = [eligible[i] for i in range(n) if finish[i] is None and eligible[i] > t + 1e-15]
        if active:
            rate = float(capacity_Bps) / float(len(active))
            dt_done = min(rem[i] / rate for i in active)
            candidates.append(t + dt_done)
        if not candidates:
            raise RuntimeError("fluid predictor has no next event")
        t_next = min(candidates)
        dt = t_next - t
        if active and dt > 0:
            rate = float(capacity_Bps) / float(len(active))
            for i in active:
                rem[i] -= rate * dt
        t = t_next
        for i in active:
            if rem[i] <= 1e-3:
                finish[i] = t
                rem[i] = 0.0
    return finish


def on_windows(period_s, duty, phase, horizon_s):
    """Configured ON intervals. A window is included when its start is before `horizon_s`.

    Phase 0 starts in the ON half. Phase 0.5 starts in the OFF half.
    """
    period_s = float(period_s)
    duty = float(duty)
    on_len = duty * period_s
    t = float(phase) * period_s
    windows = []
    guard = 0
    while t < float(horizon_s) - 1e-15:
        windows.append((t, t + on_len))
        t += period_s
        guard += 1
        if guard > 100000:
            raise RuntimeError("too many ON windows")
    return windows


def timeline_finish(work_bytes, alpha_s, line_Bps, on_windows):
    """Drain `work_bytes` starting at `alpha_s`.

    `on_windows` are configured [start, end) intervals in seconds. Inside a
    window the foreground gets half the line. Outside it gets the whole line.
    The windows are the configured background schedule, not simulated finishes.
    """
    remaining = float(work_bytes)
    t = float(alpha_s)
    bounds = []
    for start, end in on_windows:
        bounds.append(float(start))
        bounds.append(float(end))
    bounds = sorted(set(bounds))
    guard = 0
    while remaining > 1e-3:
        guard += 1
        if guard > 100000:
            raise RuntimeError("timeline predictor did not finish")
        on = False
        for start, end in on_windows:
            if float(start) <= t < float(end):
                on = True
                break
        rate = float(line_Bps) / 2.0 if on else float(line_Bps)
        nxt = None
        for bound in bounds:
            if bound > t + 1e-15:
                nxt = bound
                break
        if nxt is None:
            t += remaining / rate
            break
        step = rate * (nxt - t)
        if step >= remaining:
            t += remaining / rate
            break
        remaining -= step
        t = nxt
    return t


def percentile(values, p):
    xs = sorted(float(v) for v in values)
    if not xs:
        return 0.0
    if len(xs) == 1:
        return xs[0]
    rank = p * (len(xs) - 1)
    lo = int(rank)
    hi = min(lo + 1, len(xs) - 1)
    weight = rank - lo
    return xs[lo] * (1.0 - weight) + xs[hi] * weight


def active_timeline(starts_us, finishes_us):
    """Return rows (time_us, active_count) plus max, time-weighted mean, and overlap."""
    events = []
    for start, finish in zip(starts_us, finishes_us):
        events.append((float(start), 1))
        events.append((float(finish), -1))
    events.sort(key=lambda item: (item[0], item[1]))
    count = 0
    previous = None
    weighted = 0.0
    overlap = 0.0
    max_count = 0
    intervals = []
    rows = []
    for time, delta in events:
        if previous is not None and time > previous:
            dt = time - previous
            weighted += count * dt
            if count >= 2:
                overlap += dt
            intervals.append((previous, time, count))
        count += delta
        if count > max_count:
            max_count = count
        rows.append((time, count))
        previous = time
    span = rows[-1][0] - rows[0][0] if len(rows) >= 2 else 0.0
    mean_active = weighted / span if span > 0 else float(count)
    at_max = sum(b - a for a, b, c in intervals if c == max_count)
    fraction_at_max = at_max / span if span > 0 else 0.0
    return rows, max_count, mean_active, overlap, fraction_at_max


def parse_lines(text):
    record = None
    flows = []
    links = []
    for line in text.splitlines():
        if line.startswith("RECORD ") or line.startswith("FLOW ") or line.startswith("LINK "):
            kind, rest = line.split(" ", 1)
            item = {}
            for tok in rest.split():
                key, value = tok.split("=", 1)
                item[key] = value
            if kind == "RECORD":
                record = item
            elif kind == "FLOW":
                flows.append(item)
            else:
                links.append(item)
    return record, flows, links
