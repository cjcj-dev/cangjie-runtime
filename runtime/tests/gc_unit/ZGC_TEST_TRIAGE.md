# ZGC test-port triage

STATUS=PORTED · upstream_gtest=13/13 · upstream_jtreg=16/16 · gc_unit=285/285×2 · updated=2026-08-23

This is the live ledger for the ZGC test-port lane.  A newly added test becomes
`PORTED` only when it is in both `CMakeLists.txt` and `run_standalone.sh`, has a
deliberate-red witness, and passes the uncached gate.
`PARTIAL` means that every shared invariant was kept, while an explicitly named
ZGC structure has no Cangjie-runtime counterpart; it does not mean that an
assertion was silently weakened.

## HotSpot gtest triage

| Upstream file | Upstream assertion sites | Cangjie-runtime counterpart | Disposition | Status / inventory input |
|---|---:|---|---|---|
| `test_zForwarding.cpp` | 11 | `ZForwarding` / `ForwardingEntries`, `ZForwardingLife`, provisional `ForwardingTable` entries | Port the table power-of-two, empty/full/every-other lookup semantics exactly; additionally pin provisional/ref-count coupling | PORTED; direct 11-site port in `test_forwarding_entries.cpp` plus 13-assert provisional/ref-count witness in `test_z_forwarding_table.cpp`; new witness deliberately red, then green in both states |
| `test_zLiveMap.cpp` | 4 | `LiveInfo` + per-generation `RegionBitmap` + live-byte counters | Port first-mark liveness and one-object-page live accounting; record the absent adjacent strong/finalizable bit | PORTED/PARTIAL; 9-assert one-object-page mark/account-once test in `test_live_map.cpp`; adjacent strong/finalizable bit is a structural gap; new test deliberately red, then green in both states |
| `test_zAddress.cpp` | 30 | `ColourMask.h` + `ColourPredicates.h` | Port every supported predicate over the full epoch/colour cross-product and irregular flip schedule | PORTED; 648-value matrix checks all 14 corresponding predicates including `Remembered11`. Finalizable publication remains an inventory gap (`kFinalizableWired=false`), but upstream does not instantiate its declared Finalizable inputs |
| `test_zBitMap.cpp` | 8 | `RegionBitmap::MarkBits`, mark words, live-byte accounting | Port clear/pre-set pair idempotence and live-accounting semantics; record the absent strong/finalizable second bit | PORTED/PARTIAL; two pair-state tests with 11 assertion sites in `test_region_bitmap.cpp`; finalizable-vs-strong second-bit distinction is a structural gap; both new tests deliberately red, then green in both states |
| `test_zArray.cpp` | 20 | none; collector code uses `std::vector`, not a ZGC-owned growable-array/slice abstraction | Do not test the C++ standard library as a substitute | GAP: no corresponding collector structure |
| `test_zBitField.cpp` | 7 | `BitField<T>` in `RegionInfo.h` | Port all seven encode/decode round trips through `SetAtomicValue` / `GetAtomicValue`, including wide and pointer-compressed values | PORTED; exact seven assertion sites in `test_z_bit_field.cpp`; wide-mask UB fixed in product `FieldMask`; deliberately red, then green in both states |
| `test_zIndexDistributor.cpp` | 32 | none; work sharing uses task queues/atomic cursors, not the 16-way hierarchical claim tree | Inventory the missing claim-tree distributor | GAP: no corresponding structure |
| `test_zList.cpp` | 11 | intrusive `RegionList` / `RegionInfo` prev-next links | Port order, size, forward/reverse traversal, head removal, and tail removal semantics | PORTED/PARTIAL; same 11 assertion sites in `test_z_list.cpp`; both new tests deliberately red, then green in both states. `RegionList` has prepend, head removal, and arbitrary delete, but no insert-before/after/append API; the six-node end state and all available removal/traversal semantics are exact |
| `test_zMapper_windows.cpp` | 3 | none | Reject explicitly: Windows-only ZGC virtual-address mapper/unreserve registry; Cangjie Windows heap mapping has no such mapper | JVM/Windows-specific N/A |
| `test_zNUMA.cpp` | 9 | none | Reject explicitly: ZGC per-NUMA-node heap-share calculation; Cangjie runtime has no NUMA heap partition/policy | JVM/NUMA-specific N/A |
| `test_zPageAge.cpp` | 12 | `PageAge` / `PageAgeRange` | Keep all twelve range-endpoint assertions exactly | PORTED before this lane; direct 12-site port already present in `test_page_age.cpp` and retained unchanged |
| `test_zVirtualMemory.cpp` | 42 | none; `MemMap` owns OS mappings but there is no ZGC virtual-offset range value type | Inventory null/accessor/resize/shrink/adjacency abstraction | GAP: no corresponding structure |
| `test_zVirtualMemoryManager.cpp` | 17 | none; no discontiguous virtual reservation registry/coalescing manager | Inventory reserve/coalesce/remove-low/remove-high/remove-whole mechanism | GAP: no corresponding structure |

## jtreg behaviour-gate triage

| Upstream file | Portable behaviour-level gate? | Decision / reason |
|---|---|---|
| `TestRelocateInPlace.java` | yes, high priority | Fragmented live-set load with a forced in-place relocation arm; needs a Cangjie product injection that proves the arm fired, not merely a successful workload |
| `TestSmallHeap.java` | yes | Run the same allocation/checksum load over a heap-size matrix and require graceful OOM/success boundaries |
| `TestAlwaysPreTouch.java` | not yet | Behaviour is portable, but Cangjie exposes no equivalent pre-touch switch or committed-page counter today |
| `TestCommitFailure.java` | not yet | Requires a deterministic commit-failure injection equivalent to `ZFailLargerCommits` |
| `TestMappedCacheHarvest.java` | not yet | Depends on the currently missing ZGC mapped-cache structure |
| `TestNoUncommit.java` | not yet | Requires an uncommit policy switch and committed-capacity observation API |
| `TestUncommit.java` | not yet | Requires the missing background uncommitter plus an observable committed-capacity decrease |
| `TestZMediumPageSizes.java` | not yet | Depends on ZGC medium-page sizing/range policy, absent from the region allocator |
| `TestZForceDiscontiguousHeapReservations.java` | not yet | Depends on the missing discontiguous reservation manager and diagnostic force flag |
| `TestAllocateHeapAt.java` | no | JVM `-XX:AllocateHeapAt` option and Java process launcher contract have no Cangjie-runtime equivalent |
| `TestAllocateHeapAtWithHugeTLBFS.java` | no | JVM `AllocateHeapAt` + HugeTLBFS/large-page option contract |
| `TestGarbageCollectorMXBean.java` | no | Java Management/MXBean surface, not a collector behaviour exposed by Cangjie runtime |
| `TestMemoryMXBean.java` | no | Java Management/MXBean surface |
| `TestMemoryManagerMXBean.java` | no | Java Management/MXBean surface |
| `TestRegistersPushPopAtZGCLoadBarrierStub.java` | no for this mechanism | AArch64 C2 load-barrier stub register-save contract; Cangjie does not use that JVM/C2 stub shape |
| `TestZNMT.java` | no | JVM Native Memory Tracking accounting/output contract |
