# Ring AllReduce and model validity

Two experiment sets share this directory. Both use fixed-rate paced RoCE from `sim/`. That sender is not DCQCN. ECN and PFC are off. Routes are pinned. A row is usable only when `valid=1`: every flow finished, and drops, strips, bounces, retransmissions, NACKs, pauses, and sink gaps are zero.

The ring schedule is round-synchronous: `n` transfers run together, and the next step starts after all of them finish. It is not an NCCL pipeline. `--seed` (default 1) is applied once with htsim's `mt19937` wrapper before the topology is built. The per-sender wall-clock reseed in `sim/roce.cpp` is commented out.

## Layout

```
Makefile, main_ring.cpp, ring_*.cpp    ring harness, builds htsim_ring
preliminary-experiments/               Experiments 1, 2, 3A, 3B
model-validity/                        MV0–MV4 dumbbell suite, builds htsim_mv
```

Findings for MV0–MV4 are in `model-validity/results/RESULT_SUMMARY.md`. Preliminary results are the CSVs and the two `experiment3*_summary.txt` files next to them.

## Build

From this directory:

```bash
make
make -C model-validity
```

`make clean` and `make -C model-validity clean` remove the binaries and object files.

## Preliminary rings

`htsim_ring` is one simulation per process. Experiment scripts live under `preliminary-experiments/`. Run them from this directory.

| Experiment | Question | Run |
|---|---|---|
| 1 | Does calibrated α-β match an uncontended same-leaf ring? | `cd preliminary-experiments/experiment1 && python3 ring_experiments.py` |
| 2 | Does sharing one uplink change completion time when the static cost does not? | same script; it runs Experiment 2 only after Experiment 1 passes |
| 3A | Does minimum propagation cost pick the fastest of 120 directed rings? | `python3 preliminary-experiments/experiment3a/ring_experiment3a.py` |
| 3B | Can the propagation-cost optimum be slower than an uncontended ring? | `python3 preliminary-experiments/experiment3b/ring_experiment3b.py` |

Experiment 1 and 2 use `--mtu 9000 --queue-bytes 4608000 --seed 1` and a 2304000-byte collective. Calibration sizes are 8 through 256 packets. `--alpha-fit` and `--beta-fit` change only the printed prediction. Experiment 3A is a three-leaf fat tree, message 3456000 bytes, host 0 fixed, 120 directed rings. Experiment 3B is the same hosts split across two sites; `--experiment 4` selects that graph and the record says `experiment=3b`.

`t_sim_s` is the sender-side completion of the last flow. `static_cost_ps` is one trip of forward propagation around the ring. `l_max` is the most one-step forward paths that share a queue.

## Model validity

`htsim_mv` builds a dumbbell: each host has a 10 Gbps access link, and one 10 Gbps core is the shared bottleneck. Hop delay is 1 µs. MSS is 9000. Nominal sizes round up to a multiple of 9000. The core queue for MV0–MV3 is `(N-1) * wire_bytes + max(wire_bytes, 64 * MSS)`. MV4 uses a fixed 256 MiB queue. A data packet occupies 9064 bytes in the queue.

MV0 fits `T = α + β * wire_bytes` on one sender. Later runs load that fit and do not refit it. MV1 scales the sender count. MV2 holds aggregate rate and total bytes fixed. MV3 staggers eight starts. MV4 keeps one foreground transfer and changes the timing of a background flow with the same nominal average rate. Background ON intervals are separate `--flow` transfers, not a flag.

From `model-validity/`:

```bash
python3 scripts/run_suite.py          # MV0–MV2; leaves MV3 and MV4 rows in place
python3 scripts/run_mv3.py            # skips run ids already in runs.csv
python3 scripts/run_mv4.py            # optional case names, e.g. none constant
python3 scripts/validate_results.py
python3 scripts/analyze.py
```

Outputs are `results/runs.csv`, `flows.csv`, `links.csv`, `calibrations.csv`, `calibration_points.csv`, `plots/`, and `traces/`. Omitting `--starts-us` starts every flow at 0. The predictors in `scripts/mvlib.py` use configured starts, the core rate, the byte count, and the MV0 α. They do not read a simulated finish time.
