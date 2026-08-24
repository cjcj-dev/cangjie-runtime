# Reproducing the runtime-versus-official matrix

Run the campaign on kkk2 from a private directory. Read
`ops/design/PERF_VS_OFFICIAL_SPEC.md` before interpreting the output.

The official arm is intentionally an uncoloured official SDK. The general
toolchain preflight assumes a coloured target SDK, so its C2/C9 target-colour
checks are not applicable to this comparison. Save the full preflight output
and require C1, C3, C5, C5b, C7, and C8 to pass, including the real minimal
compile-and-run probe. Do not reinterpret unrelated SDK-tool failures as a
runtime benchmark result.

Build both immutable workload ELFs once with the official compiler:

```bash
taskset -c 80-95 ./build_workloads.sh \
  /root/.cjv/toolchains/nightly-1.2.0-alpha.20260721165458 \
  /root/perfbar-run/workloads
```

Reserve `80-95` under the `perfbar` lane and wait until competing work,
including `bareslot`, has left that core set. The protected soak must be
stopped before the timed window and continued afterwards. STOP and CONT are
deliberately separate operator commands; never use a broad process-name
match.

With the soak confirmed stopped, run the N=5 pilot:

```bash
taskset -c 80-95 ./run_matrix.sh \
  /root/perfbar-run/pilot \
  /root/perfbar-run/workloads/workloads.tsv \
  /root/.cjv/toolchains/nightly-1.2.0-alpha.20260721165458/runtime/lib/linux_x86_64_cjnative \
  /root/mainint-run/so_cert \
  5 80-95 180
```

Each round has two distinct products:

- `runs/.../rNN-subject` and `runs/.../rNN-official` are the fair comparison.
  Both leave all observation variables unset, so the timed comparison has zero
  apparatus observer work in either runtime.
- `ledger/runs/.../rNN-subject` is a supplemental subject-only observation.
  It differs from the fair subject attempt only by `MRT_GC_LOG=1`, produces the
  subject phase ledger directly on stderr, and never enters the
  subject/official performance ratio.

Odd rounds execute `subject/fair -> official/fair -> subject/ledger`; even
rounds reverse that entire order.  This balances
order for both the fair pair and the single-variable subject observer A/B.  Do not invent an
`official/ledger` attempt: the official runtime has no corresponding GcLog or
phase-timer work, so that would reproduce environmental rather than structural
symmetry.

Analyze the observer A/B separately, with the preregistered pair count:

```bash
python3 ./analyze_observer.py /root/perfbar-run/pilot \
  /root/perfbar-run/pilot/observer-analysis \
  --min-pairs 5 --bootstrap-samples 10000 --seed 20260824
```

The observer analyzer rejects a pair unless its fair and ledger metadata have
the same ELF, runtime closure, heap, cores and environment, with
`MRT_GC_LOG=UNSET -> 1` as the sole environment change.  Its result describes
the observer overhead; it is not a correction silently applied to the fair
subject/official ratio.

The zero-observer fair attempts intentionally do not produce the common
runtime/report pause log either.  Consequently `analyze.py` can still report
wall, throughput, peak RSS and correctness for this campaign, but it must mark
pause quantiles as insufficient.  Do not present this clean campaign as the
full release metric vector.  Subject phase/cycle diagnostics come from the
ledger companion; there is no structurally equivalent official phase ledger.

After the independent CONT command restores the soak, produce the pilot
matrix:

```bash
python3 ./analyze.py /root/perfbar-run/pilot \
  /root/perfbar-run/pilot/analysis \
  --min-pairs 5 --bootstrap-samples 10000 --seed 20260821
```

For a release-grade campaign, use at least 20 paired rounds and analyze with
`--min-pairs 20`. Every output directory is create-once and refuses to
overwrite evidence. `matrix.tsv` is the machine-readable decision surface;
`summary.md` is its human-readable rendering. Neither contains a weighted
aggregate score.
