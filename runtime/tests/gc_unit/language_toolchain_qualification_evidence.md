# Current pinned tuple qualification evidence

Qualification was measured on kkk2 for cangjie-runtime#1504. No std rebuild or runtime product code change was made.

The retained same-source SDK was copied from `/root/sym_cjcj_863_implement_r6027183555/keep/sdk-stage1-pre-runner`. Its selected files were checked against `SDK.lock.json`; `same-source-std.sha256` identifies std-core `47a91409…`. The SDK compiler is the supplied stage1 ELF, not the previous wrapper.

Compiler host runtime/boundscheck were copied from the pinned nightly-1.3.0-alpha.20260925001050 SDK. Host LLVM was downloaded from cjcj-dev/cjcj asset 616606322, whose archive SHA256 is `3a732a6fbab1cd09bb470374c2fb1b186668228477b92a12a98bb0227cb21806`, then checked against the Linux x86_64 host identity `30e8ba8c…`. These entity copies form the SDK `host/compiler` directory; its complete file-set identity is bound by the record.

| Measured language component | SHA256 |
|---|---|
| cjc | `a4ae3418cb59c07b273056e6f2fd9a6908c4420da15f98bf63a8dfcf62ed15ad` |
| llc | `e426bdedd85c48eaba569cccbfc3c34de4bfdf0cdff765a4209ddaa3581adc57` |
| opt | `8d8f8c5f4ab7e0da329b5ddf0b14b16f612a2a785a37892298453194c09e61f7` |
| std | `31f21961358efb67854df315a4a2494eea35da1be28af7d16f79440f844f9c44` |
| std_core | `47a91409292a5005f1dd3449bbc6b0840f89f3bbdb3ebba1235837cb768d3f60` |
| runtime | `49c3e549002409742a6b02192e67d48696ce5515deafca83d17c0e59872bd546` |

The remaining component, compiler-host directory and checker/reference hashes are the directly measured values in the qualification JSON. `evidence/measure.py` (preserved as the lane script `measure.py`) checks the selected SDK.lock identities before emitting `measured-inputs.json`; its own rc was 0.

Producer/consumer observations:

- The f228 product SO defines `ThreadLocalData::SetMutator` at `0x51bff0`. `runtime/src/Mutator/ThreadLocal.cpp:44` publishes `newMutator->tlab()` into the first TLS slot. The SO instructions at `0x51c07c`/`0x51c083` form the embedded TLAB address and store it directly into that slot.
- `runtime/src/Mutator/ThreadLocal.h:32` defines the pointer slot, and `runtime/src/Heap/z/zThreadLocalAllocBuffer.cpp:77–79` asserts TLAB/top/end offsets 0/0/8. `runtime/src/arch/x86_64_linux/CalleeSavedStub.S:59–60` puts `MRT_GetThreadLocalData()` in r15.
- The measured std-core archive defines `CJ_MCC_NewObjectFast` in `core.o` at `0x94bb0`. The linked finalizer ELF places that copy at `0x117040`: `_CGPatifHv` moves from `0x917c4` to `0x113c54`, the same `0x82490` displacement. Archive and ELF instructions read `[r15]` once, then TLAB top `[rdx]` and end `[rdx+8]` (`49 8b 17; 48 8b 02; 48 8b 4a 08`). The linked ELF also contains the fixture allocation stub at `0x81c20` with that same direct layout.

This corresponds to the TLAB top/end consumption in the reference `hotspot/cpu/x86/gc/shared/barrierSetAssembler_x86.cpp:295–305`. Cangjie publishes a pointer to its logical-thread TLAB in external TLS; HotSpot uses JavaThread fields. This observation qualifies the current AOT ABI pairing; it is not a general runtime behavior or performance claim.

One sequential N=3 batch ran the unchanged `run_finalizer_trigger.sh` on CPU domain 0-15 through `wf_kkk2.sh sh`, with core soft/hard limits 0 and `LD_DEBUG=libs`. Each fixture used `-O0 --static-std`, a 256MB heap and the same retained target runtime/boundscheck. The first sample succeeded before the remaining two ran.

| Sample | Compile rc | Runner rc | Finalized | Done | Run log SHA256 |
|---|---|---|---|---|---|
| 1 | 0 | 0 | 64 | 1 | `3fb66b24241688b7b118a7d90f208170142ea9571c24ab4e0051afb614487c07` |
| 2 | 0 | 0 | 64 | 1 | `b1054c47837f6a7f9162a06f75e94027c5f2b23971235da2f57b59aacf08b6c1` |
| 3 | 0 | 0 | 64 | 1 | `7528ad24b5abb92d1da61c8250de484cf6dc0a669a2d5a3433a3f68fb4bc1f52` |

All three ELF hashes are `b22b678cbf7d051bda4db5cd746f599a4dbbf4fdc6972668fac4557e59a3edb3`. Target SO SHA256 is `49c3e549002409742a6b02192e67d48696ce5515deafca83d17c0e59872bd546`, with the actual stamp `CJRT-COMMIT:f2283db793be1fcbe986f9a4ed5c217609dcf3ca`; target boundscheck is `4463c3457486debd598d6433893e64d43b49dcea1ba4669744a5891076ef03b9`.

Loader output in each `finalizer_trigger.build.log` records the compiler-host runtime and host LLVM; each `finalizer_trigger.run.log` records the retained f228 target runtime. `run_finalizer_trigger.sh:40–43` checks execution rc, the one completion line, and all 64 actual finalizer outputs before printing OK. Load averages were 0.45/0.31/0.68 before and 0.35/0.30/0.67 after.

Raw logs, original rc files, linked ELFs, symbol/disassembly observations and `summary.json` are preserved under `kkk2:/root/sym_cangjie_runtime_1504_implement_r6028178076/evidence/qualification/` and `local:/root/cj_build/reports/EVIDENCE-sym_cangjie_runtime_1504_implement_r6028178076/qualification/`. The lane scripts and `measured-inputs.json` are also retained. The report is `/root/cj_build/reports/REPORT-sym_cangjie_runtime_1504_implement_r6028178076.md`.

The fixed SDK runtime is a component of the input tuple. A runtime commit containing this record is admitted as a separately measured target through the unchanged colour checker; its commit stamp is not required to equal the SDK runtime hash. Thus the record has no self-referential target SO binding. See `LANGUAGE_TOOLCHAIN.md`.
