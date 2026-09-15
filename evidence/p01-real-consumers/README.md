# P01 r3 R1 real consumers

Scope: item 7 only. Earlier items 1–6 and all old evidence remain preserved.
Runtime source delivery and reports use lane `sym_cangjie_runtime_608_implement_r5683164869`.

## Producer to consumers (before any fault injection)

1. `InitCJRuntime` (`CangjieRuntimeApi.cpp:183`) creates runtime services; `CangjieRuntime.cpp:216` initializes `HeapManager`.
2. `HeapManager.cpp:20` calls `HeapImpl::Init` (`Heap/z/zHeap.cpp:212`); this calls `RegionSpace::Init`.
3. `zPageAllocator.cpp:1155` requests native memory. `zVirtualMemoryManager.cpp:445,456` tries contiguous then segmented reservations. The fixture occupies the domain and opens two 64 MiB windows; it does not provide a backend or write range globals.
4. `zPageAllocator.cpp:1189` calls `Heap::OnHeapCreated`, which publishes the real reservations (`zHeap.hpp:201`).
5. LLVM `CJBarrierLowering.cpp:685,840` consumes those ranges separately for `gcread` and `gcwrite`; calling layers are `:1079` and `:1040`.
6. The driver calls the llc-produced functions with heap dynamic/null, accessible gap/plain, and registered global storage. Runtime fallback dispatches at `CompilerCalls.cpp:328,1868`.
7. Write results are checked as stored words. Reads use independently prepared valid storage, so a faulty write cannot mask a read failure. Both read results and write words enter printed, nonfatal equality assertions.

ZGC anchors: `zVirtualMemoryManager.cpp:169-184` reserve then register; `c2/zBarrierSetC2.cpp:362,374,379` classify at store/load; `zUncoloredRoot.inline.hpp:38-59` plain roots; `zAddress.inline.hpp:806-807` store-good encoding.

## Replay

Build/test only on kkk2 through `/root/cj_build/ops/bin/wf_kkk2.sh`.
All remote products are retained under `/root/sym_cangjie_runtime_608_implement_r5683164869`.
The `abi-final-{green,cut,restored}` directories hold separate LLVM/std products. `build-canonical-arm.sh` gives each build a private mount namespace and the same visible `abi-canonical` path. This avoids path strings changing green/restored binary hashes. The scripts copy from the preserved old LLVM tree, never modify it.

`build-abi-arm.sh` is installed remotely as `build-abi-arm-final.sh`. Its inputs are the preserved LLVM tree, the frozen candidate files archive (paths/hashes in `llvm-input-manifest.json`), and `cut.diff`. The archive is reproducible from LLVM candidate `071fd4b2ec5c90c9bc09849b8fb6e80b789b5f4d`. Both real consumers are cut; no test helper is changed.

`run-consumer-arm.sh` is installed remotely as `run-consumer-arm-final.sh`. It requires each successful arm build and the final product SO from the `-finalrt` two-configuration build. It compiles the IR with that arm's llc, links that arm's std-core and real runtime/boundscheck, and retains ELF hashes, `/proc/self/maps`, three complete runs and individual carrier filters. Same core domain 0–7, each process timeout 30 seconds. Samples are causal correctness observations, not performance measurements.

Preliminary `abi-*`, `consumer-*`, `pilot*` records are kept but are not the final identity evidence: their build paths differed. Final evidence uses `abi-final-*` and `consumer-final-*` only.

## Deletions/replacements

- Removed `slot_domain_mcc_stubs.c` (three replacement functions) and its preload/build path. Product runtime MCC functions now execute.
- Removed direct writes to `g_cjHeapRange*`, synthetic range publication, heap-null dual acceptance, and the old expectation that global storage is plain.
- Added `slot_domain_read` IR consumer and eight independently printed assertions (`heap_dyn`, `heap_null`, `hole`, `global`, each read/write).
- No unit-suite expectations or known-failure exemptions were changed. The seven mainline content changes were migrated independently and checked in `main-content-check.json`; two files retain preexisting P01 changes, documented in the report.

## Final results

Complete runs N=3/arm: green 8/8 rc=0; cut 6/8 rc=1 (only `hole.write` and `hole.read`); restored 8/8 rc=0. Each carrier also ran alone. `final-summary.json` contains every assertion value and the six artifact hashes; `result-consistency.txt` records the log-derived cross-check.

Green/restored llc, frontend, test ELF, loaded std-core, runtime, and boundscheck are byte-identical. The rebuilt but **unloaded** std.math archive/shared library differ; all hashes remain in the full std receipts, and `ancillary-std-diff.json` records this limitation. This package does not claim whole-stdlib reproducibility.

Green/restored launchers were accidentally updated while Bash was reading them after static std finished. `launcher-interrupted.rc=127` is retained. `complete-shared-arm.sh` completed the remaining shared/install steps in the same private namespace (`shared-completion.rc=0`). No failed compile or load is counted as the red control.

The scripts are records of this run. To replay builds, copy them with a fresh lane/directory prefix; do not run a fresh build over these retained directories. No source/build/evidence under old Workflow or old P01 paths was deleted.
