# Stage 1 results

43 runs, seed 1, paced RoCE, ECN off, PFC off. All 43 are `valid=1`. Drops, strips, retransmissions, NACKs, ECN marks, and PFC events are zero. Calibration id `mv0-ae90d1da87a5d234`. MV1 and MV2 use that fit and do not contain any of its regression points.

Nominal sizes are rounded up to a multiple of 9000 bytes. The models below use those wire sizes. Each data packet occupies 9064 bytes in the queue (9000 + 64-byte header). `bottleneck_utilization` is core bytes served, including that header, divided by core capacity times `sim_cct`.

## Measured facts

### MV0

One sender, eight wire sizes from 72000 to 33561000 bytes.

- α = 28.524 µs
- β = 0.846241 ns/byte
- R² = 0.999999968

Residuals (`T_sim - T_fit`) span −3.840 µs to +2.135 µs. The largest absolute residual is −3.840 µs at 8397000 wire bytes, on a 7131 µs transfer.

At N = 1, `T_M0` and `T_M1_beta` are the same formula. `T_M1_capacity` is not: `1/C` is 0.800 ns/byte, and the fitted β is larger. For the 32 MiB wire size, `sim / T_M0` is 1.00003 and `sim / T_M1_capacity` is 1.0578.

### MV1

Twenty synchronized incasts. Each sender is paced at 10 Gbps into the 10 Gbps core, so ρ = N.

For the 33561000-byte messages, `sim / T_M0` is:

| N | sim / T_M0 | sim / T_M1_capacity | sim / T_M1_beta | peak queue (bytes) | sim CCT |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.00003 | 1.0578 | 1.00003 | 9,064 | 28.430 ms |
| 2 | 1.9032 | 1.00710 | 0.95209 | 32,095,624 | 54.107 ms |
| 4 | 3.8055 | 1.00711 | 0.95209 | 99,704,000 | 108.187 ms |
| 8 | 7.6100 | 1.00711 | 0.95208 | 234,902,624 | 216.346 ms |
| 16 | 15.219 | 1.00711 | 0.95208 | 505,290,808 | 432.664 ms |

The same capacity-share ratio stays near 1.006–1.007 for every N ≥ 2, including the 72000-byte messages. At N = 16 those short messages have `sim / T_M0` = 10.688, not 15.219. Their `sim / T_M1_capacity` is 1.0063.

The core queue for N = 16 and 33561000-byte messages peaks at 505,290,808 bytes. The configured queue is 536,976,000 bytes. No run drops.

For N = 1, core utilization over the makespan is 0.651 at 72000 bytes and 0.951 at 33561000 bytes. At N = 16 and 33561000 bytes it is 0.99994.

### MV2

Mode A paces every sender at 10 Gbps and sends 4203000 wire bytes each. Aggregate configured rate and total bytes both grow with N. CCT goes from 3.587 ms at N = 1 to 54.209 ms at N = 16. Aggregate achieved rate goes from 9.375 Gbps to 9.924 Gbps. The peak queue grows into the tens of megabytes.

Mode B keeps the aggregate configured rate at 10 Gbps and still sends 4203000 wire bytes per sender. CCT goes from 3.587 ms to 57.005 ms. Aggregate achieved rate stays between 9.375 and 9.437 Gbps. The peak queue at N = 16 is 135,960 bytes.

Mode C keeps the aggregate configured rate at 10 Gbps and the total nominal size at 32 MiB. MSS rounding raises total wire bytes from 33,561,000 at N = 1 to 33,696,000 at N = 16 (ratio 1.00402). CCT goes from 28.430 ms to 28.609 ms (ratio 1.00629). The peak queue at N = 16 is again 135,960 bytes.

Mode B at N = 8 and mode C at N = 8 are the same configuration: eight senders, 1.25 Gbps each, 4203000 wire bytes each. Both record CCT 28.504269 ms.

## Possible interpretations

These are readings of the table above, not separate measurements.

The isolated line is an accurate description of one paced flow on this dumbbell. The absolute residuals are a few microseconds.

Once several line-rate senders share the core, isolated α-β underestimates completion time, and the miss grows with N. For long messages the ratio is close to N. For short messages it is smaller because α is a larger fraction of the transfer.

Of the two fair-share formulas, capacity share stays within about 0.7% for every contended MV1 point. Scaling the fitted β by N overestimates those same points by about 5%. A plausible reason is that fitted β is already larger than `1/C`, so multiplying it by N also multiplies the header and ACK contribution that does not all sit on the forward core.

Mode C barely moves with N. The CCT ratio is only slightly larger than the wire-byte ratio from MSS rounding. On this lossless paced dumbbell, sender count by itself does not add a large extra cost when aggregate rate and total work are held fixed. Mode A and mode B change because the amount of traffic changes.

The short MV1 messages still match capacity share. That does not say short bursts will match a steady-state rate controller. This sender never reduces its pacing, so there is no control transient to finish before.

Worth doing next, not done here: a verified congestion-control sender, where a short synchronized burst might end before the rate has settled, and a case where the queue is shallow enough that loss or marking actually occurs.

## MV3

Eighteen more runs. Eight senders, each paced at 10 Gbps, on the same dumbbell. Nominal sizes 512 KiB, 8 MiB, and 32 MiB. Starts are evenly spaced over `S`, with `S / T_iso` in {0, 0.10, 0.25, 0.50, 1, 2}. `T_iso` is the MV0 line `α + β · wire_bytes`. All 18 runs are valid. Drops, retransmissions, NACKs, pauses, and sink gaps are zero. The `S = 0` rows for 531000 and 33561000 wire bytes match the MV1 `N = 8` completion times.

The active-set predictor holds a flow out of the core until `start + α`, then gives every eligible unfinished flow `C / k`. `M2` services the wire payload. `M2_packet` services `packet_count * 9064` bytes. Both use only the configured starts, the core rate, the byte count, and the MV0 α.

### Measured facts

Makespan barely moves as the starts spread. For 8397000 wire bytes it goes from 54151.0 µs at `S = 0` to 54243.7 µs at `S = 2 T_iso`. The 32 MiB wire size goes from 216345.9 µs to 216721.9 µs. Over the same range, `sim / T_M1_static` falls from 1.007 to about 0.80. `sim / T_M2` stays between 1.0069 and 1.0089. `sim / T_M2_packet` stays between 0.99987 and 1.0020.

Every one of the 18 runs reaches `max_active_flows = 8`. At `S = 2 T_iso` for the 8 MiB nominal size, flow 0 finishes at 15529 µs and flow 7 is scheduled to start at `2 T_iso` (about 14269 µs), so the early flow is still active when the last one starts.

Per-flow FCT for that 8 MiB size, in start order:

| S / T_iso | flow 0 | flow 3 | flow 7 |
|---:|---:|---:|---:|
| 0 | 54151 µs | 54108 µs | 54115 µs |
| 0.5 | 40478 µs | 47810 µs | 50608 µs |
| 2 | 15530 µs | 29710 µs | 39975 µs |

At `S = 0` the eight FCTs sit in a 160 µs band. At `S = 2 T_iso` they span 15530 µs to 40070 µs.

Peak core occupancy falls as the spread grows and stays large. For 8397000-byte messages it goes from 58,770,976 bytes at `S = 0` to 41,186,816 bytes at `S = 2 T_iso`. The full queue trace of the `S = T_iso` 8 MiB run has the same peak the summary row reports, 49,978,896 bytes. Contention tail for that size falls from 47017 µs at `S = 0` to 32840 µs at `S = 2 T_iso`. It does not cross zero.

### Possible interpretations

A spread measured in isolated transfer times is still short compared with the fair-share duration, which is several times `T_iso` when eight line-rate senders share one core. That is why a stagger of `2 T_iso` never separates the active set in this matrix, and why the makespan stays near the synchronized value.

The static `C/8` makespan adds that full fair-share time on top of the last start. Once the starts move apart, that sum grows while the simulated makespan does not, so the static ratio falls. The active-set fluid model follows the makespan across the sweep. Charging 9064 bytes per packet, instead of 9000, removes the roughly 0.7% residual that MV1 already showed. A few tenths of a percent remain at the widest spread.

Later flows have larger FCTs because they arrive while earlier flows are still on the core. The makespan hides that: most of it is one shared busy period, not eight isolated transfers placed end to end.

### Wider spreads

Nine more runs, at `S / T_iso` in {4, 7, 10}, for the same three sizes. The earlier eighteen rows were not resimulated. All nine are valid. `fraction_time_at_max_concurrency` is the share of the span from the first start to the last finish during which the active count equals its maximum. It was filled for the earlier rows from their recorded start and finish times.

### Measured facts

The active set changes. Every size does the same thing:

| S / T_iso | max active | mean active (8 MiB) | 8 MiB peak queue | 8 MiB makespan | 8 MiB sim / M2_packet |
|---:|---:|---:|---:|---:|---:|
| 4 | 5 | 2.784 | 23,602,656 B | 54338 µs | 1.0034 |
| 7 | 2 | 1.000 | 9,064 B | 57068 µs | 1.0059 |
| 10 | 1 | 0.727 | 9,064 B | 78471 µs | 1.0043 |

At `S = 7 T_iso` the maximum is 2, but the time spent at 2 is a small fraction of the span (1.3e-4 for 8 MiB). The peak queue is one packet. The contention tail for 8 MiB is −7.5 µs. At `S = 10 T_iso` the maximum is 1. The mean is 0.73 because the span includes idle gaps between flows; the fraction of time at that maximum is the same 0.73. `sim / M2_packet` on the nine new rows stays between 1.0034 and 1.0059. The 512 KiB and 32 MiB rows match this pattern: max active 5, then 2, then 1.

### Possible interpretations

The gap between starts is `S / 7`. At `S = 7 T_iso` that gap is one isolated transfer, so a flow that has the core to itself finishes as the next one starts. At `S = 4 T_iso` the gap is about 0.57 `T_iso` and several flows still overlap. At `S = 10 T_iso` the gap is about 1.43 `T_iso` and the flows are separate. The makespan grows at that last point because of the gaps, not because the core got slower. The active-set model still tracks the makespan after the flows separate.

## MV4

Nine runs. One 32 MiB foreground transfer, paced at 10 Gbps, on the same dumbbell. `T_fg_iso` is the existing MV0 measurement for this wire size, 28430.078 µs. The foreground queue holds 268435456 bytes. ECN and PFC are off. A background ON interval is a separate paced flow with its own start time and byte count. The OFF gap is the time until the next flow starts. All nine runs are valid. Drops, retransmissions, NACKs, pauses, and sink gaps are zero.

`M0` is the MV0 line. `M1_average` gives the foreground `C − 5 Gbps = 5 Gbps` on every shared-background case, and the full core on the no-background and disjoint cases. `M2_timeline` is the configured schedule: 10 Gbps while the background is OFF, 5 Gbps while it is ON and both senders offer 10 Gbps. The constant shared case is ON for the whole foreground, so `M1_average` and `M2_timeline` are the same prediction there. The packet version serves `packet_count * 9064` bytes. `background_overlap_time_us` is the serialization time of background packets on the foreground core during the foreground, not the wall-clock span of the overlap.

The no-background run finishes in 28430.078 µs, the same value as the MV0 point.

### Measured facts

| Pattern | Phase | Foreground FCT | Achieved background during foreground | Overlap bytes on the foreground core | sim / M1 | sim / M2 | sim / M2 packet |
|---|---:|---:|---:|---:|---:|---:|---:|
| NONE |  | 28430.078 µs | 0 | 0 | 1.0578 | 1.0578 | 1.0503 |
| CONSTANT |  | 40591.182 µs | 3.333 Gbps | 16,913,424 | 0.7555 | 0.7555 | 0.7502 |
| BURSTY_FAST | 0 | 41425.166 µs | 3.468 Gbps | 17,955,784 | 0.7710 | 1.1474 | 1.1414 |
| BURSTY_FAST | 0.5 | 41383.716 µs | 3.445 Gbps | 17,819,824 | 0.7703 | 1.1688 | 1.1606 |
| BURSTY_MEDIUM | 0 | 41389.296 µs | 3.462 Gbps | 17,910,464 | 0.7704 | 1.0475 | 1.0375 |
| BURSTY_MEDIUM | 0.5 | 41244.871 µs | 3.274 Gbps | 16,877,168 | 0.7677 | 1.2136 | 1.2068 |
| BURSTY_SLOW | 0 | 54078.357 µs | 4.996 Gbps | 33,772,464 | 1.0066 | 1.0066 | 0.9995 |
| BURSTY_SLOW | 0.5 | 28429.178 µs | 0 | 0 | 0.5291 | 1.0577 | 1.0503 |
| DISJOINT_CONSTANT |  | 28413.979 µs | 4.759 Gbps | 0 | 1.0572 | 1.0572 | 1.0497 |

The 5 Gbps prediction used by `M1_average` on the shared cases is 53726 µs. The isolated `M0` line is 28429 µs.

For the medium, phase-0 schedule the first configured ON window is 0 to 14215 µs and carries 17,775,000 bytes at 10 Gbps. That flow actually finishes at 28663 µs. The next configured ON starts at 28430 µs.

On the disjoint run the foreground core carries 33,799,656 bytes, one foreground packet train, and its peak is 9064 bytes. The second core carries 71,587,472 bytes.

### Answers

1. The constant 5 Gbps background does not behave like a 5 Gbps foreground. The foreground finishes in 40591 µs, against 53726 µs for that prediction (ratio 0.756). During the foreground the background contributes 3.333 Gbps on the shared core, not 5.

2. The bursty schedules do not all match that constant run, and they do not match each other. Fast and medium finish near 41.3–41.4 ms. The slow schedule finishes in 54.1 ms or 28.4 ms depending on phase. The constant run is 40.6 ms.

3. The foreground time depends on the burst period. At 0.1 `T_fg_iso` and at 1.0 `T_fg_iso` the four runs sit between 41245 and 41425 µs. At 10 `T_fg_iso` the two phases are 54078 µs and 28429 µs.

4. Phase changes the foreground by 41 µs on the fast period and by 144 µs on the medium period. On the slow period it changes the foreground from 54078 µs to 28429 µs.

5. The timeline model accounts for the slow-period gap. At phase 0 both models predict the shared 5 Gbps line, and `sim / M2_packet` is 0.9995. At phase 0.5 the average model still predicts 53726 µs (ratio 0.529) while the timeline model predicts a full line and `sim / M0` is 1.0000. On the fast period the timeline ratio is about 1.15 and the average ratio is about 0.77, so the timeline model is closer and still off. On medium phase 0 the timeline ratio is 1.047. The configured OFF interval is not idle: the first medium burst runs until 28663 µs, through the gap that the schedule marks as OFF.

6. The disjoint control stays near the isolated run: 28414 µs versus 28430 µs. Overlap on the foreground core is 0 bytes. The background is served on the other core.

### Possible interpretations

A FIFO core splits service by arrival rate. The constant background offers 5 Gbps and the foreground offers 10, so the foreground gets about two thirds of the core rather than the leftover 5 Gbps. That lines up with 40591 µs against a two-thirds drain near 40.3 ms. `M1_average` assumes the leftover. The simulator does not do that when the two pacers differ.

When both pacers offer 10 Gbps, the same FIFO split is half and half. The slow phase-0 foreground sits inside one such ON interval, which is why it matches the 5 Gbps line. The slow phase-0.5 foreground ends before the background starts, which is why it matches the isolated line. Subtracting the long-run average of 5 Gbps cannot tell those two apart. The configured timeline can.

On the shorter periods the distinction collapses for a different reason. A burst sized for 10 Gbps takes about twice as long once it shares the core, so it is still on the core during the interval the schedule calls OFF. The foreground therefore sees background traffic through most of its life (about 3.3–3.5 Gbps achieved), and the two phases finish within a few hundred microseconds of each other. The timeline model, which trusts the configured OFF windows, is then early.
