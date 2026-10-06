# Ring AllReduce harness

`htsim_ring` runs one simulation per process: a pinned two-host transfer, or one round-synchronous ring AllReduce. `ring_experiments.py` is the Experiment 1 / 2 suite. It writes `ring_results/all_runs.csv`.

Build from this directory:

```bash
make
```

## Files

Added here:

- `main_ring.cpp` — parses the CLI and calls `run_ring_experiment`.
- `ring_runner.cpp` / `ring_runner.h` — builds the fat tree, pins routes, installs barriers and RoCE flows, and prints the `RECORD` line.
- `ring_planner.cpp` / `ring_planner.h` — selects the ring, pins each forward route, and computes `L_max`, `H_max`, and the static path cost. `TspPlanner` is unused by Experiments 1 and 2.
- `ring_schedule.cpp` / `ring_schedule.h` — expands a ring into `2(n-1)` steps of `n` transfers of `s/n` bytes.
- `ring_metrics.cpp` / `ring_metrics.h` — records each queue's run-long peak occupancy and bytes served.
- `ring_experiments.py` — fits α and β from the P2P sweeps, then runs the three collectives.
- `Makefile` — compiles those sources and links `libhtsim.a` plus the existing fat-tree objects.

Modified outside this directory:

- `sim/roce.cpp` — the per-sender `srand(time(NULL))` is commented out, with a note, so a run no longer reseeds from the wall clock.

## Reproduce

Run these from this directory. Shared flags are `--mtu 9000 --queue-bytes 4608000 --seed 1`. Message size for every collective is `2304000` bytes (`n = 4`, 64 packets of 9000 per chunk). Calibration sizes are 8, 16, 32, 64, 128, and 256 packets.

Experiment 1 P2P sweep, hosts 0→1 on one leaf. Repeat with `--bytes` set to `72000`, `144000`, `288000`, `576000`, `1152000`, and `2304000`:

```bash
./htsim_ring --mode p2p --experiment 1 --pin none --n 4 --bytes 72000 --mtu 9000 --queue-bytes 4608000 --seed 1
```

Experiment 1 collective. The fit values are the ordinary-least-squares result of that sweep (`T = α + β · bytes`):

```bash
./htsim_ring --mode collective --experiment 1 --pin none --n 4 --bytes 2304000 --mtu 9000 --queue-bytes 4608000 --seed 1 --alpha-fit 1.8907682910447674e-05 --beta-fit 8.4541983279041307e-10
```

Experiment 2 P2P sweep, hosts 0→2 through spine 0. Same six sizes:

```bash
./htsim_ring --mode p2p --experiment 2 --pin spine0 --n 4 --bytes 72000 --mtu 9000 --queue-bytes 4608000 --seed 1
```

Experiment 2 spread and stack collectives:

```bash
./htsim_ring --mode collective --experiment 2 --pin spread --n 4 --bytes 2304000 --mtu 9000 --queue-bytes 4608000 --seed 1 --alpha-fit 3.7385481164179161e-05 --beta-fit 8.4733107319553014e-10
./htsim_ring --mode collective --experiment 2 --pin stack --n 4 --bytes 2304000 --mtu 9000 --queue-bytes 4608000 --seed 1 --alpha-fit 3.7385481164179161e-05 --beta-fit 8.4733107319553014e-10
```

`--alpha-fit` and `--beta-fit` change only the printed prediction. Omit them and `t_sim_s` is unchanged; `t_fit_s` and `slowdown_fit` become 0. `python3 ring_experiments.py` runs this sequence and stops before Experiment 2 if Experiment 1 is invalid or `|slowdown_fit - 1| > 0.05`.

## `all_runs.csv`

One row per invocation. Times are seconds except where the name says otherwise. The fit columns are `na` or 0 on calibration rows because the fit is passed in only on the collective invocations.

| Column | Meaning |
|---|---|
| `t_sim_s` | Sender-side completion of the last flow, in seconds. The flow is done when the cumulative ACK covers its size. |
| `t_fit_s` | `2(n-1)·α_fit + 2(n-1)/n · bytes · β_fit`, in seconds. Zero when no fit was passed. |
| `t_theory_s` | Same formula with theoretical α (sum of forward pipe delays on one edge) and β (`1 /` link bytes per second). |
| `slowdown_fit` | `t_sim_s / t_fit_s`. |
| `static_cost_ps` | Sum of forward-path propagation around the ring once, in picoseconds. One step, not `2(n-1)` steps, and not the ACK return. |
| `l_max` | Most logical edges of one step whose forward data path shares one queue. ACK paths are not counted. |
| `h_max_s` | `max over queues of (bytes offered to that queue by one step) / (queue rate in bytes/s)`, in seconds. |
| `peak_queue_bytes` | Largest queue occupancy seen at enqueue or service, in bytes. |
| `valid` | `1` only when every flow finished and drops, strips, bounces, retransmissions, NACKs, pauses, and sink gaps are all zero. |

## Randomness

`--seed` (default `1`) is applied with `srand` before the topology or any flow is created. The binary defines its own `srand`, `srandom`, `rand`, and `random`; those are htsim's `mt19937` wrappers in `sim/config.cpp`, so this does not use libc's generator. `srandom` is the same function.

That generator is drawn three times on this path: each switch's hash salt during topology build, each `RoceSrc` path id (`random() % 256`, unused once the route is pinned), and the RoCE pacing jitter `rand() % (spacing / 10)` at flow start and between packets. The same `--seed` repeats `t_sim_s`. Seed 1 matches the checked-in calibration row (`8.0403402000000002e-05` for the 72000-byte Experiment 1 transfer); seed 2 does not.

## What this is not

This schedule is the homogeneous α-β ring: `n` transfers run together, and the next step starts only after all of them finish. It is not an NCCL pipeline, and chunks are not sent ahead across steps. Routes are pinned, including the ACK path; there is no ECMP, spraying, or PLB. The sender is fixed-rate RoCE at the 10 Gbps link rate, paced with the 64-byte ACK included in the gap, plus that jitter. ECN and PFC are off. A usable row has `valid=1`, so the recorded time contains no loss recovery. Switch latency is 0. α theory is forward propagation only; the fitted α is larger because completion includes pacing and the ACK return.
