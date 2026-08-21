# GC performance versus the official runtime

This document freezes the measurement method for release condition ③. It
defines a matrix, not a single benchmark score. A gain in one workload or
metric must never hide a loss in another.

## 1. Scope and comparison question

The experiment asks whether the current `cangjie_runtime` main runtime is
faster than the official SDK runtime for programs produced by the official
SDK compiler. It does not compare compiler code generation.

The only intentional arm difference is the runtime closure selected through
`LD_LIBRARY_PATH`:

- `official`: the read-only official SDK runtime and bounds-check libraries;
- `subject`: the read-only certified current-main runtime and matching
  bounds-check library.

Each pair executes the same immutable workload ELF. Every workload ELF is
compiled once by the official SDK `cjc`, before timing starts. Recompiling one
ELF per arm, applying an ABI shim, or modifying either shared installation
changes the question and invalidates the comparison.

## 2. Preregistered workload set

The minimum release matrix contains both of these deterministic workloads:

| ID | Workload | Stress shape | Completion record |
|---|---|---|---|
| `SD` | `survival_dense.cj` | high survival with a rotating large root set | `SURVIVAL_DENSE_OK` |
| `NW` | `natural_wave_notime.cj` | repeated allocation and mutation waves | `NATURAL_WAVE_OK` |

The run manifest freezes, before any timed attempt, each source SHA-256,
official compiler SHA-256, ELF SHA-256, completion marker, work-unit count,
heap matrix, and runtime-library SHA-256. Workloads may be added only by
creating a new manifest and a new result matrix; post-result workload
selection is forbidden.

The required heap matrix is `256MB` and `1GB`. The pilot uses at least five
paired rounds per workload and heap. A release claim requires at least 20
valid paired rounds per cell. Pilot results are descriptive and cannot by
themselves freeze condition ③.

## 3. Metrics

All values are reported separately for every `workload × heap` cell and arm.

| Metric | Definition | Better direction |
|---|---|---|
| wall | elapsed seconds for the complete process, measured by `/usr/bin/time` | lower |
| throughput | preregistered work units divided by wall seconds | higher |
| pause p50 | nearest-rank p50 over cross-runtime pause events | lower |
| pause p99 | nearest-rank p99 over cross-runtime pause events | lower |
| pause p999 | nearest-rank p99.9 over cross-runtime pause events | lower |
| peak memory | `/usr/bin/time` maximum resident set size in KiB | lower |

Cross-runtime pause events are only the log fields emitted by both runtimes:
`stw time <N> us` and `light sync time <N> us`. Subject-only phase timers are
diagnostic columns and are not mixed into the comparison. Quantiles pool
events within an arm, but confidence resampling is clustered by paired run so
that many pauses from one process do not become independent observations.

Throughput is retained because it is a required user-facing metric. For these
fixed-work workloads it is the inverse view of wall, not an independent vote.
The table must label it `derived_from_wall=true`.

The result also reports correctness, run count, pause-event count, terminal
stdout SHA-256, GC-cycle count, and timeout/exit classification. These are
validity and diagnostic columns, not numbers that may be traded for speed.

## 4. Fairness and execution protocol

1. Run only on kkk2, bound to the reserved CPU set `80-95`. Record hostname,
   CPU model, kernel, NUMA placement, start/end time, and the reservation
   ledger snapshot.
2. Before the timed window, stop the protected soak using its recorded process
   group and the task-authorized STOP command. STOP and the later CONT are two
   independent commands. Confirm the clean window, and always issue CONT in
   cleanup. Never use a broad process-name match.
3. Execute pairs serially. Odd rounds run `subject → official`; even rounds run
   `official → subject`. Do not run the two arms concurrently.
4. Use the same CPU set, ELF, heap, timeout, locale, logging settings, and
   otherwise empty environment for both arms. The arm-specific runtime path
   is the only intended difference.
5. Do not set GC tuning variables for one or both arms. In particular, do not
   change full-young-scan, concurrency, trigger, worker, or force-serial knobs
   to improve a losing cell. Record every `MRT_*` and `cjHeapSize` value passed
   by the driver; omitted variables are recorded as `UNSET`.
6. Separate stdout from runtime/report logs. A run is correct only when it
   exits zero, `/usr/bin/time` records exit zero, contains the registered
   completion marker, and its stdout SHA-256 equals all correct attempts of
   both arms for that workload.
7. Check `MemAvailable` before each pair. If it is below 40 GiB, record
   `DEFERRED_LOW_MEM` and do not count the pair. Do not use `ulimit -v`.
8. Record every attempted run. Apparatus failures, timeouts, crashes, and
   deferred pairs are never silently replaced. A replacement uses a new round
   identifier and remains visible in `attempts.tsv`.
9. The pilot and release run use a fresh output directory and refuse to
   overwrite prior evidence. Workload binaries and referenced evidence remain
   immutable; intermediate compiler files may be removed after their hashes
   are recorded.

## 5. Ratios and confidence

The analyzer uses paired-round cluster bootstrap with a recorded deterministic
seed and at least 10,000 resamples. Each resample draws paired rounds with
replacement, preserving both processes and all pause events belonging to a
drawn round.

For lower-is-better metrics the reported ratio is `subject / official`. For
throughput it is also `subject / official`, with the direction explicitly
marked higher-is-better. The point estimate is a ratio of arm aggregates:
median wall, median throughput, pooled nearest-rank pause quantile, or median
peak RSS. The analyzer reports the 2.5th and 97.5th percentiles of the
bootstrap ratio distribution.

Tail resolution is visible. For each pause quantile the table includes event
counts and `tail_observations = ceil((1 - q) × events)`. A p99 or p999 that is
effectively selected from only the largest few events remains reported but is
marked `LOW_TAIL_RESOLUTION`; it must not be described as precise.

## 6. Decision vocabulary

Correctness is a prerequisite. If either arm has an incorrect attempt, mixed
stdout hashes, fewer valid pairs than required, or no comparable pause event,
the affected metric is `INVALID` or `INSUFFICIENT`, never a performance win.

Per metric:

- lower-is-better `SUPERIOR`: the 95% ratio interval is wholly below `1.0`;
- lower-is-better `INFERIOR`: the 95% ratio interval is wholly above `1.0`;
- higher-is-better uses the reverse inequalities;
- otherwise: `INCONCLUSIVE`.

Point estimates are always printed even when the confidence status is
inconclusive. This prevents a noisy favorable median from being presented as
a proven win and prevents an unfavorable cell from disappearing.

Per `workload × heap`, the result is a vector:
`wall / throughput / p50 / p99 / p999 / peak-memory`. It is
`UNIFORMLY_SUPERIOR` only when all non-derived metrics are `SUPERIOR` and
correctness is valid. It is `UNIFORMLY_INFERIOR` when all non-derived metrics
are `INFERIOR` and correctness is valid. It is `MIXED` when the vector contains
both favorable and unfavorable or inconclusive entries. No weighted average,
geometric mean, or single score is produced.

The unqualified sentence “GC is superior to the official runtime” is allowed
only if every preregistered workload and heap cell is
`UNIFORMLY_SUPERIOR`. Otherwise the report must name every winning, losing,
inconclusive, invalid, and low-tail-resolution cell explicitly.

## 7. Required artifacts

A reproducible result directory contains:

- `manifest.tsv`: frozen workload, compiler, ELF, runtime, heap, core, and
  environment identities;
- `attempts.tsv`: one row per process attempt, including invalid attempts;
- `metrics.tsv`: per-arm aggregates and confidence inputs;
- `matrix.tsv`: one row per metric per workload and heap, with ratio, interval,
  direction, status, and tail resolution;
- `summary.md`: the same matrix for humans, without collapsing it to one score;
- per-attempt `meta`, stdout, stderr, timing, runtime log, report log, and exit
  classification;
- the exact driver/analyzer revision and invocation.

All identities include SHA-256 and provenance stamps when available. A missing
stamp is written as `NO_PROVENANCE_STAMP`, never omitted.
