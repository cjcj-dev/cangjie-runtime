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
