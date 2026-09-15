# P08 function / data migration map — preparation, not acceptance

Runtime input `91f3dcc232201d4ad98ec3af6f10165edb950416`; first saved cleanup `2f1bb137974c6848ba10fe6180176e65d09a054c`. #607 input read at `efde143b243b88a6ef5733bbdd3077f53ba11680`, WIP; not treated as accepted final integration. LLVM main reading is observational until advisor freezes its coordinate. Reference root is `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`; hashes and exact query results: `migration-inputs.json`.

## Correspondence and ordering

| ZGC anchor | Existing runtime anchor (input SHA) | Migration / status |
|---|---|---|
| zBarrier.hpp:74, :79–183 | Heap/z/zBarrier.hpp:22, :130 | Replace instance and Collector/RememberedSet references with static barrier class; split access operations into BarrierSet. Pending. |
| zBarrier.inline.hpp:318 | zBarrier.inline.hpp:21; zBarrier.cpp:151,246,308; zMark.cpp:192 | One templated funnel for load/store/mark; fast(o) then make_load_good(o), slow exactly once, color/self_heal iff p. #607 routes must migrate as functions. Pending. |
| zBarrier.inline.hpp:41,72 | ObjectModel/RefField.h:244,257; zBarrier.cpp:37 | Move self_heal to barrier, remove HealSite/HealNull/HealSlot wrapper. allow_null guard before retries, relaxed CAS loop, debug-only monotonicity checks. Pending. |
| zBarrier.inline.hpp:110,294,306; zBarrier.cpp:49,53 | Heap/WCollector/WCollector.h:241,269; zCollectedHeap.hpp:267 | Remap-generation belongs to static barrier, takes colored word and returns generation pointer; generation provides forwarding/remap/relocate operations. Need preserve current real forwarding routes. Pending. |
| zBarrier.inline.hpp:346 | zRelocate.cpp:1088 | Promoted field remap must use no-relocate operation, not make_load_good that can initiate movement. Pending. |
| zBarrier.inline.hpp:363–403 | HeapSlot::GetFieldValue plus inline predicates and per-function lambdas | One checked atomic read; seven distinct fast predicates. Pending. |
| zBarrier.cpp:61–144,280 | zBarrier.cpp:308 weak/phantom branch | Separate keep_alive/blocking weak/phantom slow paths; reference liveness/phase semantics must match reference, not literal zero phase count. Advisor pending. |
| zBarrier.inline.hpp:456–589 | zBarrier ReadReference/Weak/Phantom/Static; reference-processor cleaning | Preloaded entry family with explicit weak/no_keep_alive/clean routes; native phantom used by #644. Pending. |
| zBarrier.inline.hpp:591–689; zBarrier.cpp:146–250 | #607 zBarrier.inline.hpp:121–177 and zBarrier.cpp slow methods; main zMark.cpp TraceRefField | Preserve independent Finalizable root seed vs old field young-target null return, remset re-remember path, young-to-old major/nonmajor semantics. Await shared-function coordinate. |
| zBarrier.inline.hpp:695–740; zBarrier.cpp:253–277 | zBarrier.cpp StoreBarrier/NativeStoreBarrier/RecordCrossGenEdge | Three store entries and slow paths; heap slow buffer-or-mark_and_remember; native slow no remset. One remember function. Pending. |
| zBarrierSet.inline.hpp:69,131–171,232–327,576–616 | CompilerCalls.cpp:320–448,1868; zBarrier atomic methods | Decorator-selected heap/native and strong/weak/no_keepalive routes. Preserve P01 stack/global/heap legal slot classification at compiler boundary. Preloaded LLVM ABI required. Pending. |
| zBarrierSet.inline.hpp:329–361,473–543 | zBarrier.cpp:61 CopyReferenceSlots and bulk consumers | Ordered direct per-slot store barrier/load barrier/store-good, overlap direction handling; remove snapshot. One plain value-copy path per PLAN I6. Pending. |
| zBarrierSet.cpp:230–244 | zBarrier WriteI8..F64 / WriteField; MObject::Store, MArray::SetPrimitiveElement | Wrapper deletion saved; direct primitive Field write now in consumers. Build/test pending. |
| zBarrierSetRuntime.cpp:29–144 | CompilerCalls CJ_MCC calls + LLVM mapping | Preloaded load wrappers and selectors; store slow only discharges barrier, generated code writes. Clone row is PLAN I7 (no Cangjie clone intrinsic), not fabricated callable API. Pending. |
| zThreadLocalData.hpp:35–133; zBarrierSet.cpp:246–271 | zThreadLocalData.hpp:13; ThreadLocal.cpp:38–65,107 | One data structure: 5 masks, buffer pointer, embedded stacks[2], invisible root pointer; attach/create/destroy/offsets. Replace lazy allocation; real attach paths include GC workers and foreign threads. Pending. |
| zStackWatermark.cpp:188–203 | Mutator.cpp:956,979 → StackWatermark::TryBegin | TLS mask refresh and buffer on_new_phase must occur before resuming compiled accesses. P10 owns full watermark; minimal P08 seam requested. Pending. |
| zUncoloredRoot.inline.hpp:34–153 | zMark.cpp EnumAndTagRawRoot; zRelocate ResolveMinorReference; RefField root writeback | Color provided externally; make_load_good(null,color) determines direct safe vs relocation; callback then plain writeback. Full root protocol pending P10 seam. |
| zUncoloredRoot.inline.hpp:34–67; PLAN I3/I4 | zBarrier ReadPlainRoot/WritePlainRoot and scheduler/sync/plain root consumers | Mutator/plain access wrappers removed at saved checkpoint; GC root processing unchanged. Build/test pending. |
| zStoreBarrierBuffer.hpp:33–73; inline:33–59 | zStoreBarrierBuffer.hpp:29–85; inline:10–37 | Entry=(p,prev), bases parallel; byte-offset current with index accessor; Add flush only full; buffer_for_store three decisions. Pending. |
| zStoreBarrierBuffer.cpp:72–114; zRelocate.cpp:1056 | Current entries carry pBase/pOffset at Add; no generation-forwarding based installation | Find base via P02 page.find_base before source map retires; lock and last_installed_color claim; both relocate-start and on_new_phase call sites. Pending. |
| zStoreBarrierBuffer.cpp:130–244 | RemapPendingField + MarkAndRemember, :22–104 | Separate relocate→remember→mark; relocate tests remap_bits, remset flip scans field, old SATB phase gate preserved. Pending advisor. |
| zStoreBarrierBuffer.cpp:246–316 | Flush(:106), VisitEntries/Contains in MutatorManager.cpp:748–763 | Distinct flush and is_in(remap); remove test-only observations/public pending iteration. Pending. |
| zBarrierSetAssembler.cpp:28–34; x86:310–318 | LLVM CJBarrierLowering.cpp:725,827,866,1067 | Runtime mask offsets exported from TLS layout; weak read mark-bad fast path; keep P01 real reserved-range guard until legal slot routing replaces it. Pending LLVM coordinate. |
| zBarrierSetC2.cpp:383–398 | LLVM CJBarrierLowering.cpp:1019–1022 | Atomic swap/CAS store-bad fast path plus healing slow; preserve return-value uncolor semantics. Pending. |
| ZGC generated store fast path | LLVM CJBarrierLowering.cpp:880–890 | Remove PostWrite call; runtime CompilerCalls.cpp CJ_MCC_PostWriteRefField removed in same bundle. Pending. |
| ZGC color predicates | LLVM CJBarrierLowering.cpp:324–656,1044 | Delete GCPhaseCheck and option; native static stores still barriered. Pending. |

## Deletion ledger

Saved, unbuilt: `Barrier::WriteI8/WriteI16/WriteI32/WriteI64/WriteF32/WriteF64`, `WriteField` specializations, their six Windows exports, `ReadPlainRoot/WritePlainRoot` wrappers. Existing typed slot primitives remain until the complete raw-root protocol migration.

Pending with replacement consumers: instance Barrier members; Load/Store/NativeStore/Mark duplicated funnels; CopyReferenceSlots snapshot; WriteGeneric/ReadGeneric duplicated routing; forwarding provenance/holder diagnostics; HealSite/HealNull/HealSlot/ZgcSelfHeal and root wrappers; MarkStaleGuard/TRACECOV/FindTo recheck; buffer pBase/pOffset/Add overloads/RemapPendingField/Discard/Pending/VisitEntries/test observations; Heap/Barrier directory; GCPhaseCheck/PostWrite compiler/runtime exports; allocator y2y producers with P05/P09 coordination. This ledger does not claim their deletion is complete.

## Deterministic validation plan — not executed

- Funnel: real compiler/runtime load of old-colored slot, observe returned object and stored new slot word; second load must preserve result and take fast arm. A product self_heal removal must fail second-slot-state target, not first-load precondition. Run filter and full suite with identical ELF; preserve baseline-entry cut separately for entry_cut_check.
- Mark route: reuse #607 real allocation/store/mark-start + legal GC-worker API tests. Old field → young must not old-mark/follow/heal; independent Finalizable seed and young remset consumers remain positive controls.
- Buffer: real store producer → relocation-start install → watermark on_new_phase → remembered scanning. Cut base installation only after reconstructing a real moved holder; assert remapped slot/target, with nonmoving and empty-buffer controls. Removing fake collector observation is not evidence of product behavior.
- Compiler: generated load/preloaded ABI and zero-call good-store/atomic blocks, plus force bad-color to observe real slow result. Keep P01 stack/global/non-contiguous reserved-range tests. Removing PostWrite alone is not sufficient without remset/old-value slow evidence.
- Build default/testable in shared build slots; default + filler (same default ELF), testable and actual OHOS attempt. Managed finalizer_trigger/segmented_array_managed N≥3 only after joint LLVM/runtime/std identity exists. No baseline or historical result borrowed as current pass.
