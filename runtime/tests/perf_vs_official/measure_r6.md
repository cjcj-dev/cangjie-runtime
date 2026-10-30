# #820 r6 parameterized preparation

`measure_r6.mjs` replaces the private historical r6 shell recipe's hardcoded
paths, without modifying the frozen runtime product. It uses modern ESM with
the existing zx-style shebang; it can also be invoked with `node`.

The four cells are NW256, NW512, SD256, SD512. Each cell runs official then ours,
N paired samples (minimum 5). Cells run sequentially, instead of the historical
two simultaneous cells. This change removes inter-cell contention; it does not
reproduce the historical scheduling. No retry, best-of selection or resume is
implemented. An existing output directory is refused. A failed process or
invalid sample stops the remaining campaign and preserves its files.

## Entry points

```text
node measure_r6.mjs plan CONFIG.json
node measure_r6.mjs sample official NW256 HISTORICAL_SAMPLE_DIRECTORY
node measure_r6.mjs summarize MEASURE_DIRECTORY 5
```

`plan` only produces argv arrays and never checks/executes the workload files.
`sample` validates process rc, time exit, wall/RSS, load, checksum and timers.
`summarize` consumes exactly N raw directories for both arms in all four cells;
it refuses incomplete/failed campaigns rather than comparing empty populations.
Its `numericCriteriaMet` result is only the numeric comparison, not independent
producer/ABI/loader/workload qualification or a release approval.

The separately authorized future measurement entry is
`node measure_r6.mjs run CONFIG.json`. **TASK02:00 does not authorize running it.**
The outer existing wf/claim/scope must supply the admitted quiet 16-core window,
memory/storage/log allowance, core soft/hard 0 and Swap0. Preserve the 64–79
protected range and check SMT siblings; the runner does not claim or stop other
owners. `run` checks its scope includes the requested cores, records affinity,
rejects load1 > 50, captures all eight selected input identities, and runs the
original time → timeout300/k10 → taskset → env-i → ELF recipe. The recorded rc is
the actual time/timeout chain rc; no unobserved inner signal or exit is invented.
Actual loader/producer/source/optimization qualification is a prerequisite
from the normal build/qualification stage, not inferred from the hashes.

The config contains normal invocation parameters only:

```json
{
  "n": 5,
  "output": "/approved-new-task/output",
  "cwd": "/approved-new-task",
  "cores": {"256": "144-159", "512": "160-175"},
  "official": {"NW": "/fixed/official-nw", "SD": "/fixed/official-sd", "library": "/fixed/official-runtime-lib"},
  "ours": {"NW": "/fixed/ours-nw", "SD": "/fixed/ours-sd", "library": "/fixed/ours-runtime-lib"}
}
```

These are placeholders, not selected inputs or CPU authorization. Both arms
must have normally produced same-source `-O2 --static-std` workloads, each with
its own matching std/runtime. Old O0/unknown-optimization ELFs are not substitutes.
New Stage3/ELF and loader qualification remains outstanding at this handoff.

## Rulers retained

- Ours: `cjConcGCThreads=4`, `MRT_GC_LOG=1`. Official: `MRT_REPORT` and
  `MRT_LOG_LEVEL=v`, without those two subject knobs. Heap 256/512MB, verify-off.
- Ours: sum only `rec=phase kind=pause ns`, reusing the existing
  `gclog_schema.py` current-schema parser and lifecycle validation. This is not
  `held_ns`, wait+held, collection wall or concurrent/subphase duration.
  A truncated or aborted ledger is rejected for this full performance ruler;
  the minor contract's separately reported tail truncation is not relaxed here.
- Official: r5's four-timer proxy (enum roots, MarkSatbBuffer, Preforward,
  ForwardFromRegions). Comma-separated microseconds are parsed; each observed
  Begin/total cycle must contain exactly all four timers. No fallback to other
  official timer names, no double-reading report+stderr, no missing-as-zero.
  This proxy is not a claim to measure all actual official STW time.
- NW: exactly 12 ordered unique waves and every independent historical golden
  checksum from `tools/gate_nwdet.py`, then the final marker/checksum. SD: its
  expected final marker/checksum. Two equally wrong outputs never pass.
- Per cell/metric: noise = max(range(official), range(ours)); median advantage
  strictly beyond noise is better/worse, boundary is noise-tie. NW wall must be
  better, SD wall must not be worse, all four STW proxies must be better.
  The superseded r5 all-four-wall-better rule is not silently reused.

## Limited validation and its boundaries

`test_measure_r6.mjs HISTORICAL_MEASURE R6_SUMMARY R5_SUMMARY NEW_OUTPUT`
only runs parser/plan CLI calls. It never calls `run`, compiles or starts a
workload. Old r6 official raw logs are replayed against retained r6 values;
old subject rc127 remains a failure. Removed/malformed/duplicate timers,
truncation, checksum divergence, nonzero rc and noise controls use task-local
copies, with independent SD controls and restoration against the untouched
originals. Current subject lifecycle positive data is explicitly a synthetic
existing-schema control, not an actual performance sample.

The original r5 raw measure directory is absent at the recorded path. Its
retained forty measured scalar summaries test only the noise calculation; this
does not prove raw-log compatibility or qualify an old/new subject producer.
No new receipt/schema/qualification flag is produced. Existing workload
qualification holds, minor C3, independent review and real measurement remain
separate obligations. See the lane's `minor-perf-1031/C3-SOURCE-CONTRACT.md` for
the precise current allocation-observation gap; no runtime counter was added.
