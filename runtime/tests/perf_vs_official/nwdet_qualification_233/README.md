# NW qualification recheck for #233

Qualification remains **false** after the post-P3 recheck on frozen runtime `c92077024634a07274e67659e254853cc3f3e200`. All five real workload processes started, returned `-6` (SIGABRT), and were rejected by the gate (`rc=1`). Each first fatal check was `ColourCensus.cpp:26`, with `plain_holder_type=std.core:OutOfMemoryError`. This records the first stop, not its root cause. The merge gate's NW segment remains unqualified.

## Reproduction and identities

The recipe uses official nightly `1.3.0-alpha.20260831010012` cjc, a private SDK copy, and the frozen runtime/bounds pair built by the retained `gate_build.py` with `MRT_GC_UNIT_TESTS=OFF`. Both gate build and workload compilation returned `0`. The compiler host uses the official runtime; target linking uses the new pair. The workload is compiled with `-O2 --static-std --save-temps -V` and a saved linker map. The official static stdlib archive is retained; this does not establish an all-coloured stdlib closure.

The current gate and saved prior gate have identical SHA256 (`184f3403edb29b2cd83bcf46f63a6029d15feaeefdf2b4edcf9a2f4296ca595f`). Because the gate exits on the first invalid sample, `requalify.py` invokes the unchanged gate five times with `N=1`, distinct output directories and the same retained ELF and SOs. Total requested and started samples are both five. Each invocation uses reserved `CORES=0-15`, `TMO=300`, required heap `256MB`, and all six verify settings from the gate. Observational heaps are not reached after failure.

`manifest.json` binds every sample to its ELF, both SOs, runtime stamp and original stderr line, and records hashes of each gate-copied SO pair. It retains both previous attempts in `prior_attempt`. The evidence archive at `kkk2:/root/sym_cangjie_runtime_233_implement_r5643268249/evidence.tgz` includes `build.sh`, `compile.sh`, `requalify.py`, link map, input hashes, original results, logs, process rc, both SOs and the ELF. Local and remote before/after uptime are preserved in the report evidence directory.

## Three-coordinate first-stop comparison

| Coordinate | Started samples | Process rc / gate rc | First check | First holder type |
|---|---:|---|---|---|
| `dea244c0918ae4b874d4490e12dc97268b78371a` (historical pre-P2) | 1 (then stopped; originally requested 3) | -6 / 1 | `ColourCensus.cpp:26` | `std.core:OutOfMemoryError` |
| `1882559969fbc91135cb9a2759129dec9c2c2838` (historical post-P2) | 5 | -6 / 1 for each | `ColourCensus.cpp:26` for each | `std.core:OutOfMemoryError` for each |
| `c92077024634a07274e67659e254853cc3f3e200` (post-P3) | 5 | -6 / 1 for each | `ColourCensus.cpp:26` for each | `std.core:OutOfMemoryError` for each |

This is a first-stop comparison. The runtime and relinked ELF changed; historical core reservations were `16-31` and `32-47`, while this run reserved `0-15`. Historical arms were not rerun. These observations do not isolate P2/P3 causality or support a performance comparison. Existing product follow-up is #190.

## Gate controls and deployment boundary

The existing fixture suite was rerun: correct → wave8-only checksum deviation → restored returns `0 → 1 → 0`. In the middle arm, normal exit, complete records and required counts all pass before the wave8 assertion rejects admission. Missing/duplicate records, missing completion, abnormal exit and missing heap count are independently rejected. `gate-controls.json` retains the same structured results; this run's originals are in the evidence archive. These controls prove the gate only, not real NW qualification or a runtime repair.

`qualification.patch` is a review proposal for the ops repository's `state/nwdet_qualification.json`. No shared canonical asset, SDK or qualification setting is deployed. The proposal retains `qualified=false`, `since` and `workloads`; it updates only the reason. The gate reads `NWDET_QUALIFICATION_FILE`, defaulting to `/root/nwdet_qualification.json` (`gate_nwdet.py:41,543-551`). The private true file used during requalification permits measurement only; it does not authorize shared deployment.

ZGC obligation: `test/hotspot/jtreg/gc/z/TestSmallHeap.java:43-66` requires successful process completion. NW still fails that requirement. This run does not add or replace a product mechanism or port a new test. Product check: `runtime/src/Heap/Verify/ColourCensus.cpp:24-34`, called through `VerifyHeap.cpp:389` and `ColourCensus.cpp:99`.
