# D09 第四轮：独立测试观测边界

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

搬前基线 `3439312e41310da5c961ffa2025642e19081142f`；本轮起点 `3c4f0da73f1127ff3e0c120b9406e18586dedc1c`。本表优先于旧版文件级职责锚；不声明独立测试定义存在 ZGC 对应函数。

## 定义对应与归属

这些独立测试定义在 ZGC 无对应文件／函数，均留旧层，归 #228 / A06 或所属机制包删除。普通 inline 保持原 inline 与宏可见性；内部状态头片段由唯一产品 TU 在原状态可见位置 include，保持匿名/static 链接，不是新增机制。

| 定义 | 本轮起点 | 退回定义 | ZGC / 归属 |
|---|---|---|---|
| `contains_for_test` | `runtime/src/Heap/z/zForwardingAllocator.hpp:36` | `runtime/src/Heap/Allocator/ForwardingAllocator.h:11` | 无独立对应；#228 / A06 / 所属机制包 |
| `GetStaticRootCountForTesting` | `runtime/src/Heap/z/zHeap.cpp:151` | `runtime/src/Heap/HeapTestObservations.h:5` | 无独立对应；#228 / A06 / 所属机制包 |
| `GetStaticRootCountForTesting` | `runtime/src/Heap/z/zHeap.cpp:257` | `runtime/src/Heap/HeapTestObservations.h:10` | 无独立对应；#228 / A06 / 所属机制包 |
| `SetHeapStartForTesting` | `runtime/src/Heap/z/zHeap.hpp:139` | `runtime/src/Heap/Heap.h:11` | 无独立对应；#228 / A06 / 所属机制包 |
| `SetMarkClosureObserverForTest` | `runtime/src/Heap/z/zMarkStack.cpp:70` | `runtime/src/Heap/Collector/MarkStripe.cpp:15` | 无独立对应；#228 / A06 / 所属机制包 |
| `ObserveMarkClosureForTest` | `runtime/src/Heap/z/zMarkStack.cpp:74` | `runtime/src/Heap/Collector/MarkStripe.cpp:19` | 无独立对应；#228 / A06 / 所属机制包 |
| `SetStorageObserver` | `runtime/src/Heap/z/zMarkStack.cpp:88` | `runtime/src/Heap/Collector/MarkStripe.h:17` | 无独立对应；#228 / A06 / 所属机制包 |
| `InjectDispelCountForTest` | `runtime/src/Heap/z/zPage.hpp:782` | `runtime/src/Heap/Allocator/RegionInfo.h:157` | 无独立对应；#228 / A06 / 所属机制包 |
| `Pending` | `runtime/src/Heap/z/zPageAllocator.hpp:173` | `runtime/src/Heap/Allocator/AllocationStallQueue.h:11` | 无独立对应；#228 / A06 / 所属机制包 |
| `EnqueuedCount` | `runtime/src/Heap/z/zPageAllocator.hpp:178` | `runtime/src/Heap/Allocator/AllocationStallQueue.h:20` | 无独立对应；#228 / A06 / 所属机制包 |
| `DequeuedCount` | `runtime/src/Heap/z/zPageAllocator.hpp:183` | `runtime/src/Heap/Allocator/AllocationStallQueue.h:29` | 无独立对应；#228 / A06 / 所属机制包 |
| `SatisfiedCount` | `runtime/src/Heap/z/zPageAllocator.hpp:188` | `runtime/src/Heap/Allocator/AllocationStallQueue.h:38` | 无独立对应；#228 / A06 / 所属机制包 |
| `FailedCount` | `runtime/src/Heap/z/zPageAllocator.hpp:193` | `runtime/src/Heap/Allocator/AllocationStallQueue.h:47` | 无独立对应；#228 / A06 / 所属机制包 |
| `ProcessReferences` | `runtime/src/Heap/z/zReferenceProcessor.cpp:193` | `runtime/src/Heap/Collector/ReferenceProcessor.h:18` | 无独立对应；#228 / A06 / 所属机制包 |
| `SetBeforeWeakCleanCasForTest` | `runtime/src/Heap/z/zReferenceProcessor.cpp:198` | `runtime/src/Heap/Collector/ReferenceProcessor.h:23` | 无独立对应；#228 / A06 / 所属机制包 |
| `ContextScope` | `runtime/src/Heap/z/zRelocate.cpp:2091` | `runtime/src/Heap/Collector/zRelocateTestObservations.h:22` | 无独立对应；#228 / A06 / 所属机制包 |
| `~ContextScope` | `runtime/src/Heap/z/zRelocate.cpp:2094` | `runtime/src/Heap/Collector/zRelocateTestObservations.h:25` | 无独立对应；#228 / A06 / 所属机制包 |
| `RouteLookupForTest` | `runtime/src/Heap/z/zRelocate.cpp:2083` | `runtime/src/Heap/Collector/zRelocateTestObservations.h:14` | 无独立对应；#228 / A06 / 所属机制包 |
| `ResetFlipTouchCountsForTest` | `runtime/src/Heap/z/zRememberedSet.cpp:527` | `runtime/src/Heap/Barrier/RememberedSet.h:19` | 无独立对应；#228 / A06 / 所属机制包 |
| `ReadFlipTouchCountsForTest` | `runtime/src/Heap/z/zRememberedSet.cpp:533` | `runtime/src/Heap/Barrier/RememberedSet.h:25` | 无独立对应；#228 / A06 / 所属机制包 |
| `MeasureClearBufferTouchesForTest` | `runtime/src/Heap/z/zRememberedSet.cpp:538` | `runtime/src/Heap/Barrier/RememberedSet.h:30` | 无独立对应；#228 / A06 / 所属机制包 |
| `RootCountForTesting` | `runtime/src/Heap/z/zRootsIterator.cpp:72` | `runtime/src/Heap/Collector/TracingCollector.cpp:341` | 无独立对应；#228 / A06 / 所属机制包 |
| `NotifyFlushObserver` | `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:25` | `runtime/src/Heap/Barrier/StoreBarrierBufferTestObservations.h:14` | 无独立对应；#228 / A06 / 所属机制包 |
| `SetFlushObserverForTest` | `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:62` | `runtime/src/Heap/Barrier/StoreBarrierBufferTestObservations.h:25` | 无独立对应；#228 / A06 / 所属机制包 |
| `LastProcessedColorForTest` | `runtime/src/Heap/z/zStoreBarrierBuffer.hpp:73` | `runtime/src/Heap/Barrier/StoreBarrierBuffer.h:11` | 无独立对应；#228 / A06 / 所属机制包 |
| `SetY2yDirtyHolderMergeHookForTest` | `runtime/src/Heap/z/zThreadLocalAllocBuffer.hpp:182` | `runtime/src/Heap/Allocator/AllocBuffer.h:11` | 无独立对应；#228 / A06 / 所属机制包 |
| `SetYoungAllocBlackHandoffHookForTest` | `runtime/src/Heap/z/zThreadLocalAllocBuffer.hpp:197` | `runtime/src/Heap/Allocator/AllocBuffer.h:21` | 无独立对应；#228 / A06 / 所属机制包 |
| `FireHandoffHook` | `runtime/src/Heap/z/zThreadLocalAllocBuffer.hpp:209` | `runtime/src/Heap/Allocator/AllocBuffer.h:30` | 无独立对应；#228 / A06 / 所属机制包 |

## 既有退回项与明确例外

| 项 | 锚与判定 |
|---|---|
| SetAllocationStallTestHooks / Pending / Enqueued / Dequeued / Satisfied / Failed | `runtime/src/Heap/Allocator/zPageAllocator.cpp:105` 起六个既有定义；本轮保持原位置，头内五个队列计数读取也已退回 AllocationStallQueue.h |
| ZGranuleMap<T>::ResetForTest | `runtime/src/Heap/z/zGranuleMap.hpp:73`，按 advisor 类型/模板例外保留，归页表机制包 / #228；无 ZGC 测试包装对应 |
| ArenaForTest / SetGhostLookupTestHook | 新布局只有声明，定义此前已留 `Heap/Allocator/zForwardingTable.cpp` / `Heap/Allocator/zPage.cpp` |
| 产品内消费 | Create/Destroy、MarkAndRemember、CleanWeakReferenceWithResult、ClearBuffer、TryForwardObject、StallAllocation 原函数体保持；对应 ZGC 产品文件职责，不把回调消费误记为独立测试定义 |
| HeapImpl 测试计数 | TU 私有类型的观测通过 HeapTestObservations.h 在类和实例之后 include，类型未移出 TU |

裁定：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_484_implement_r5654131619-20260913T151727Z.md`、`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_484_implement_r5654131619-20260913T151807Z.md`、`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_484_implement_r5654131619-20260913T151950Z.md`。

## 全量命中 → 去向

尺：在本轮起点全体 Heap/z 文件逐行检索 `Observer|ForTest|_test_|TestHook|MRT_.*_OBSERVE|for_test|ForTesting`，补含后缀变体；表内保留声明、成员、混合产品体消费与模板例外，不能以 grep 命中数代替独立定义数。

| 原命中 | 原文 | 去向 | 逐处分类 |
|---|---|---|---|
| `runtime/src/Heap/z/zForwardingAllocator.hpp:35` | `bool contains_for_test(const void* address, size_t size) const` | `runtime/src/Heap/Allocator/ForwardingAllocator.h:11` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zForwardingTable.hpp:136` | `static const ForwardingAllocator* ArenaForTest(Generation gen);` | `runtime/src/Heap/z/zForwardingTable.hpp:136` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zGranuleMap.hpp:73` | `void ResetForTest() { Reset(); }` | `runtime/src/Heap/z/zGranuleMap.hpp:73` | 模板可见性例外；归页表机制包 / #228 |
| `runtime/src/Heap/z/zHeap.cpp:151` | `size_t GetStaticRootCountForTesting() { return staticRootTable.RootCountForTesting(); }` | `runtime/src/Heap/HeapTestObservations.h:5` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zHeap.cpp:256` | `size_t Heap::GetStaticRootCountForTesting()` | `runtime/src/Heap/HeapTestObservations.h:10` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zHeap.cpp:258` | `return g_heapInstance->GetStaticRootCountForTesting();` | `runtime/src/Heap/HeapTestObservations.h:10` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zHeap.hpp:34` | `static size_t GetStaticRootCountForTesting();` | `runtime/src/Heap/z/zHeap.hpp:34` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zHeap.hpp:139` | `static void SetHeapStartForTesting(MAddress startAddr) { heapStartAddr = startAddr; }` | `runtime/src/Heap/Heap.h:11` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zMark.cpp:1358` | `~ClosureObservation() { ObserveMarkClosureForTest(&objects); }` | `runtime/src/Heap/z/zMark.cpp:1358` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zMark.cpp:1810` | `ObserveMarkClosureForTest(nullptr);` | `runtime/src/Heap/z/zMark.cpp:1810` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zMark.hpp:288` | `// Observers see the product result after dispatch; neither supplies work.` | `runtime/src/Heap/z/zMark.hpp:288` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zMarkStack.cpp:25` | `std::atomic<MarkStripeStack::StorageObserver> storageObserver{nullptr};` | `runtime/src/Heap/Collector/MarkStripe.h:11` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zMarkStack.cpp:43` | `if (const auto observer = storageObserver.load(std::memory_order_acquire)) {` | `runtime/src/Heap/z/zMarkStack.cpp:42 / runtime/src/Heap/z/zMarkStack.cpp:56` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zMarkStack.cpp:57` | `if (const auto observer = storageObserver.load(std::memory_order_acquire)) {` | `runtime/src/Heap/z/zMarkStack.cpp:42 / runtime/src/Heap/z/zMarkStack.cpp:56` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zMarkStack.cpp:67` | `std::atomic<MarkClosureObserver> g_markClosureObserver{nullptr};` | `runtime/src/Heap/Collector/MarkStripe.cpp:12` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zMarkStack.cpp:69` | `void SetMarkClosureObserverForTest(MarkClosureObserver observer)` | `runtime/src/Heap/Collector/MarkStripe.cpp:15` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zMarkStack.cpp:71` | `g_markClosureObserver.store(observer, std::memory_order_release);` | `runtime/src/Heap/Collector/MarkStripe.cpp:16` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zMarkStack.cpp:73` | `void ObserveMarkClosureForTest(const std::vector<BaseObject*>* objects)` | `runtime/src/Heap/Collector/MarkStripe.cpp:19` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zMarkStack.cpp:75` | `auto observer = g_markClosureObserver.load(std::memory_order_acquire);` | `runtime/src/Heap/Collector/MarkStripe.cpp:20` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zMarkStack.cpp:87` | `void MarkStripeStack::SetStorageObserver(StorageObserver observer)` | `runtime/src/Heap/Collector/MarkStripe.h:17` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zMarkStack.cpp:89` | `storageObserver.store(observer, std::memory_order_release);` | `runtime/src/Heap/Collector/MarkStripe.h:18` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zMarkStack.hpp:49` | `using StorageObserver = void (*)(const MarkStripeStack*, const MarkStackEntry*, size_t, bool);` | `runtime/src/Heap/z/zMarkStack.hpp:49` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zMarkStack.hpp:50` | `MRT_EXPORT static void SetStorageObserver(StorageObserver observer);` | `runtime/src/Heap/z/zMarkStack.hpp:50` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zMarkStack.hpp:166` | `using MarkClosureObserver = void (*)(const std::vector<BaseObject*>*);` | `runtime/src/Heap/z/zMarkStack.hpp:166` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zMarkStack.hpp:167` | `MRT_EXPORT void SetMarkClosureObserverForTest(MarkClosureObserver observer);` | `runtime/src/Heap/z/zMarkStack.hpp:167` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zMarkStack.hpp:168` | `void ObserveMarkClosureForTest(const std::vector<BaseObject*>* objects);` | `runtime/src/Heap/z/zMarkStack.hpp:168` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPage.hpp:600` | `using GhostLookupTestHook = void (*)(RegionInfo*);` | `runtime/src/Heap/z/zPage.hpp:600` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPage.hpp:601` | `MRT_EXPORT static void SetGhostLookupTestHook(GhostLookupTestHook hook);` | `runtime/src/Heap/z/zPage.hpp:601` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPage.hpp:602` | `MRT_EXPORT static size_t GhostLookupTestHookCalls();` | `runtime/src/Heap/z/zPage.hpp:602` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPage.hpp:770` | `static std::atomic<GhostLookupTestHook> ghostLookupTestHook;` | `runtime/src/Heap/z/zPage.hpp:770` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPage.hpp:771` | `static std::atomic<size_t> ghostLookupTestHookCalls;` | `runtime/src/Heap/z/zPage.hpp:771` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPage.hpp:772` | `static void RunGhostLookupTestHook(RegionInfo* region);` | `runtime/src/Heap/z/zPage.hpp:772` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPage.hpp:781` | `static void InjectDispelCountForTest()` | `runtime/src/Heap/Allocator/RegionInfo.h:157` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zPage.inline.hpp:1257` | `RunGhostLookupTestHook(region);` | `runtime/src/Heap/z/zPage.inline.hpp:1257` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPageAllocator.cpp:59` | `void RunRemapWindowTestHook(unsigned point, RegionInfo* region, BaseObject* object);` | `runtime/src/Heap/z/zPageAllocator.cpp:59` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPageAllocator.cpp:543` | `#if defined(MRT_ALLOCATION_STALL_OBSERVE)` | `runtime/src/Heap/z/zPageAllocator.cpp:543 / runtime/src/Heap/z/zPageAllocator.cpp:549 / runtime/src/Heap/z/zPageAllocator.cpp:563` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.cpp:544` | `if (allocationStallBeforeWaveTestHook) {` | `runtime/src/Heap/z/zPageAllocator.cpp:544` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zPageAllocator.cpp:545` | `allocationStallBeforeWaveTestHook(*this);` | `runtime/src/Heap/z/zPageAllocator.cpp:545` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zPageAllocator.cpp:549` | `#if defined(MRT_ALLOCATION_STALL_OBSERVE)` | `runtime/src/Heap/z/zPageAllocator.cpp:543 / runtime/src/Heap/z/zPageAllocator.cpp:549 / runtime/src/Heap/z/zPageAllocator.cpp:563` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.cpp:550` | `if (allocationStallGcTestHook) {` | `runtime/src/Heap/z/zPageAllocator.cpp:550` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zPageAllocator.cpp:551` | `allocationStallGcTestHook(*this);` | `runtime/src/Heap/z/zPageAllocator.cpp:551` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zPageAllocator.cpp:563` | `#if defined(MRT_ALLOCATION_STALL_OBSERVE)` | `runtime/src/Heap/z/zPageAllocator.cpp:543 / runtime/src/Heap/z/zPageAllocator.cpp:549 / runtime/src/Heap/z/zPageAllocator.cpp:563` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.cpp:564` | `const bool satisfied = request.Wait(allocationStallBeforeWaitTestHook` | `runtime/src/Heap/z/zPageAllocator.cpp:564` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zPageAllocator.cpp:565` | `? [this] { allocationStallBeforeWaitTestHook(*this); }` | `runtime/src/Heap/z/zPageAllocator.cpp:565` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zPageAllocator.hpp:19` | `#define MRT_ALLOCATION_STALL_OBSERVE 1` | `runtime/src/Heap/z/zPageAllocator.hpp:19` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.hpp:108` | `#if defined(MRT_ALLOCATION_STALL_OBSERVE)` | `runtime/src/Heap/z/zPageAllocator.hpp:108 / runtime/src/Heap/z/zPageAllocator.hpp:144 / runtime/src/Heap/z/zPageAllocator.hpp:159 / runtime/src/Heap/z/zPageAllocator.hpp:171 / runtime/src/Heap/z/zPageAllocator.hpp:184 / runtime/src/Heap/z/zPageAllocator.hpp:549 / runtime/src/Heap/z/zPageAllocator.hpp:869` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.hpp:144` | `#if defined(MRT_ALLOCATION_STALL_OBSERVE)` | `runtime/src/Heap/z/zPageAllocator.hpp:108 / runtime/src/Heap/z/zPageAllocator.hpp:144 / runtime/src/Heap/z/zPageAllocator.hpp:159 / runtime/src/Heap/z/zPageAllocator.hpp:171 / runtime/src/Heap/z/zPageAllocator.hpp:184 / runtime/src/Heap/z/zPageAllocator.hpp:549 / runtime/src/Heap/z/zPageAllocator.hpp:869` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.hpp:159` | `#if defined(MRT_ALLOCATION_STALL_OBSERVE)` | `runtime/src/Heap/z/zPageAllocator.hpp:108 / runtime/src/Heap/z/zPageAllocator.hpp:144 / runtime/src/Heap/z/zPageAllocator.hpp:159 / runtime/src/Heap/z/zPageAllocator.hpp:171 / runtime/src/Heap/z/zPageAllocator.hpp:184 / runtime/src/Heap/z/zPageAllocator.hpp:549 / runtime/src/Heap/z/zPageAllocator.hpp:869` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.hpp:171` | `#if defined(MRT_ALLOCATION_STALL_OBSERVE)` | `runtime/src/Heap/z/zPageAllocator.hpp:108 / runtime/src/Heap/z/zPageAllocator.hpp:144 / runtime/src/Heap/z/zPageAllocator.hpp:159 / runtime/src/Heap/z/zPageAllocator.hpp:171 / runtime/src/Heap/z/zPageAllocator.hpp:184 / runtime/src/Heap/z/zPageAllocator.hpp:549 / runtime/src/Heap/z/zPageAllocator.hpp:869` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.hpp:204` | `#if defined(MRT_ALLOCATION_STALL_OBSERVE)` | `runtime/src/Heap/z/zPageAllocator.hpp:108 / runtime/src/Heap/z/zPageAllocator.hpp:144 / runtime/src/Heap/z/zPageAllocator.hpp:159 / runtime/src/Heap/z/zPageAllocator.hpp:171 / runtime/src/Heap/z/zPageAllocator.hpp:184 / runtime/src/Heap/z/zPageAllocator.hpp:549 / runtime/src/Heap/z/zPageAllocator.hpp:869` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.hpp:568` | `#if defined(MRT_ALLOCATION_STALL_OBSERVE)` | `runtime/src/Heap/z/zPageAllocator.hpp:108 / runtime/src/Heap/z/zPageAllocator.hpp:144 / runtime/src/Heap/z/zPageAllocator.hpp:159 / runtime/src/Heap/z/zPageAllocator.hpp:171 / runtime/src/Heap/z/zPageAllocator.hpp:184 / runtime/src/Heap/z/zPageAllocator.hpp:549 / runtime/src/Heap/z/zPageAllocator.hpp:869` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.hpp:569` | `using AllocationStallTestHook = std::function<void(RegionManager&)>;` | `runtime/src/Heap/z/zPageAllocator.hpp:550` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPageAllocator.hpp:570` | `MRT_EXPORT void SetAllocationStallTestHooks(AllocationStallTestHook beforeWave,` | `runtime/src/Heap/z/zPageAllocator.hpp:551` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zPageAllocator.hpp:571` | `AllocationStallTestHook requestGc,` | `runtime/src/Heap/z/zPageAllocator.hpp:552` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zPageAllocator.hpp:572` | `AllocationStallTestHook beforeWait);` | `runtime/src/Heap/z/zPageAllocator.hpp:553` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPageAllocator.hpp:888` | `#if defined(MRT_ALLOCATION_STALL_OBSERVE)` | `runtime/src/Heap/z/zPageAllocator.hpp:108 / runtime/src/Heap/z/zPageAllocator.hpp:144 / runtime/src/Heap/z/zPageAllocator.hpp:159 / runtime/src/Heap/z/zPageAllocator.hpp:171 / runtime/src/Heap/z/zPageAllocator.hpp:184 / runtime/src/Heap/z/zPageAllocator.hpp:549 / runtime/src/Heap/z/zPageAllocator.hpp:869` | 保留宏配置或混合产品宏分支；不改变宏条件 |
| `runtime/src/Heap/z/zPageAllocator.hpp:889` | `AllocationStallTestHook allocationStallBeforeWaveTestHook;` | `runtime/src/Heap/z/zPageAllocator.hpp:870` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPageAllocator.hpp:890` | `AllocationStallTestHook allocationStallGcTestHook;` | `runtime/src/Heap/z/zPageAllocator.hpp:871` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zPageAllocator.hpp:891` | `AllocationStallTestHook allocationStallBeforeWaitTestHook;` | `runtime/src/Heap/z/zPageAllocator.hpp:872` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zReferenceProcessor.cpp:19` | `std::function<void()> g_beforeWeakCleanCasForTest;` | `runtime/src/Heap/Collector/ReferenceProcessor.h:11` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zReferenceProcessor.cpp:144` | `if (g_beforeWeakCleanCasForTest) {` | `runtime/src/Heap/z/zReferenceProcessor.cpp:139` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zReferenceProcessor.cpp:145` | `g_beforeWeakCleanCasForTest();` | `runtime/src/Heap/z/zReferenceProcessor.cpp:140` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zReferenceProcessor.cpp:197` | `void ReferenceProcessor::SetBeforeWeakCleanCasForTest(std::function<void()> hook)` | `runtime/src/Heap/Collector/ReferenceProcessor.h:23` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zReferenceProcessor.cpp:199` | `g_beforeWeakCleanCasForTest = std::move(hook);` | `runtime/src/Heap/Collector/ReferenceProcessor.h:24` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zReferenceProcessor.hpp:57` | `static void SetBeforeWeakCleanCasForTest(std::function<void()> hook);` | `runtime/src/Heap/z/zReferenceProcessor.hpp:57` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:134` | `void RunRemapWindowTestHook(unsigned point, RegionInfo* region, BaseObject* object);` | `runtime/src/Heap/z/zRelocate.cpp:132` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:421` | `if (GetCycleReason() == GC_REASON_YOUNG) RunRemapWindowTestHook(1, nullptr, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:419` | 产品函数内部消费或注释；保留原位置与函数体 |
| `runtime/src/Heap/z/zRelocate.cpp:1232` | `RunRemapWindowTestHook(7, nullptr, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:1230` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:1272` | `RunRemapWindowTestHook(4, nullptr, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:1270` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:2082` | `WCollector::RouteLookupTestResult WCollector::RouteLookupForTest(BaseObject* fromObj)` | `runtime/src/Heap/Collector/zRelocateTestObservations.h:14` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zRelocate.cpp:2112` | `RunRemapWindowTestHook(8, ghostFromRegion, obj);` | `runtime/src/Heap/z/zRelocate.cpp:2090` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:2225` | `RunRemapWindowTestHook(6, copyPage, obj);` | `runtime/src/Heap/z/zRelocate.cpp:2203` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:2310` | `RunRemapWindowTestHook(12, region, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:2288` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:2317` | `RunRemapWindowTestHook(10, region, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:2295` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:2321` | `RunRemapWindowTestHook(5, region, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:2299` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:2632` | `RunRemapWindowTestHook(9, region, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:2610 / runtime/src/Heap/z/zRelocate.cpp:2767` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:2697` | `RunRemapWindowTestHook(11, region, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:2675 / runtime/src/Heap/z/zRelocate.cpp:2835` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:2789` | `RunRemapWindowTestHook(9, region, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:2610 / runtime/src/Heap/z/zRelocate.cpp:2767` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:2857` | `RunRemapWindowTestHook(11, region, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:2675 / runtime/src/Heap/z/zRelocate.cpp:2835` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRelocate.cpp:2893` | `RunRemapWindowTestHook(3, region, nullptr);` | `runtime/src/Heap/z/zRelocate.cpp:2871` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRememberedSet.cpp:526` | `void RememberedSet::ResetFlipTouchCountsForTest()` | `runtime/src/Heap/Barrier/RememberedSet.h:19` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zRememberedSet.cpp:532` | `RememberedSet::FlipTouchCounts RememberedSet::ReadFlipTouchCountsForTest() const` | `runtime/src/Heap/Barrier/RememberedSet.h:25` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zRememberedSet.cpp:537` | `RememberedSet::FlipTouchCounts RememberedSet::MeasureClearBufferTouchesForTest(size_t buffer)` | `runtime/src/Heap/Barrier/RememberedSet.h:30` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zRememberedSet.cpp:540` | `ResetFlipTouchCountsForTest();` | `runtime/src/Heap/Barrier/RememberedSet.h:32` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zRememberedSet.cpp:544` | `return ReadFlipTouchCountsForTest();` | `runtime/src/Heap/Barrier/RememberedSet.h:36` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zRememberedSet.hpp:98` | `void ResetFlipTouchCountsForTest();` | `runtime/src/Heap/z/zRememberedSet.hpp:98` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRememberedSet.hpp:99` | `FlipTouchCounts ReadFlipTouchCountsForTest() const;` | `runtime/src/Heap/z/zRememberedSet.hpp:99` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRememberedSet.hpp:100` | `FlipTouchCounts MeasureClearBufferTouchesForTest(size_t buffer);` | `runtime/src/Heap/z/zRememberedSet.hpp:100` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zRootsIterator.cpp:71` | `USize StaticRootTable::RootCountForTesting()` | `runtime/src/Heap/Collector/TracingCollector.cpp:341` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zRootsIterator.hpp:29` | `USize RootCountForTesting();` | `runtime/src/Heap/z/zRootsIterator.hpp:29` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:22` | `thread_local StoreBarrierFlushObserver g_flushObserver = nullptr;` | `runtime/src/Heap/Barrier/StoreBarrierBufferTestObservations.h:11` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:24` | `void NotifyFlushObserver(StoreBarrierFlushEvent event, const StoreBarrierEntry& entry)` | `runtime/src/Heap/Barrier/StoreBarrierBufferTestObservations.h:14` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:26` | `if (g_flushObserver != nullptr) {` | `runtime/src/Heap/Barrier/StoreBarrierBufferTestObservations.h:15` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:27` | `g_flushObserver(event, entry);` | `runtime/src/Heap/Barrier/StoreBarrierBufferTestObservations.h:16` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:61` | `void StoreBarrierBuffer::SetFlushObserverForTest(StoreBarrierFlushObserver observer)` | `runtime/src/Heap/Barrier/StoreBarrierBufferTestObservations.h:25` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:63` | `g_flushObserver = observer;` | `runtime/src/Heap/Barrier/StoreBarrierBufferTestObservations.h:26` | 独立观测状态 / 实现块退回旧层；#228 / A06 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:94` | `NotifyFlushObserver(StoreBarrierFlushEvent::PREVIOUS_RETIRED, entry);` | `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:76` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:98` | `NotifyFlushObserver(StoreBarrierFlushEvent::PREVIOUS_INVALID, entry);` | `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:80` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:118` | `NotifyFlushObserver(StoreBarrierFlushEvent::SLOT_REMEMBERED, entry);` | `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:100` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.hpp:47` | `using StoreBarrierFlushObserver = void (*)(StoreBarrierFlushEvent, const StoreBarrierEntry&);` | `runtime/src/Heap/z/zStoreBarrierBuffer.hpp:47` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.hpp:73` | `uintptr_t LastProcessedColorForTest() const { return lastProcessedColor; }` | `runtime/src/Heap/Barrier/StoreBarrierBuffer.h:11` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zStoreBarrierBuffer.hpp:76` | `static void SetFlushObserverForTest(StoreBarrierFlushObserver observer);` | `runtime/src/Heap/z/zStoreBarrierBuffer.hpp:76` | 保留声明 / 状态成员或产品体内消费；定义去向见下表 |
| `runtime/src/Heap/z/zThreadLocalAllocBuffer.hpp:181` | `void SetY2yDirtyHolderMergeHookForTest(Y2yDirtyHolderMergeHook hook, void* context)` | `runtime/src/Heap/Allocator/AllocBuffer.h:11` | 独立定义退回旧层；#228 / A06 / 所属机制包 |
| `runtime/src/Heap/z/zThreadLocalAllocBuffer.hpp:196` | `void SetYoungAllocBlackHandoffHookForTest(HandoffHook hook, void* context)` | `runtime/src/Heap/Allocator/AllocBuffer.h:21` | 独立定义退回旧层；#228 / A06 / 所属机制包 |

## 删除清单与测试

本轮删除新布局中的上述独立定义块，函数／机制／宏／测试均不删除。原目录头恢复为独立观测片段，因而不再属于退休路径；它们的 include 是不变量 2 的有意保留。退休路径清单按实际文件存在性生成，并与搬前同尺 grep 作阳性对照。windows_x86_64_exports.def 保持原内容。

测试沿用 gc_z_layout.md 的七个 test_z*.cpp 搬移，无新增/删除/改名；注册集合差见本棒证据 test-and-switch-sets.json。按 alignment_mode 不跑 unit/切刀/门。

## 同尺限制

旧词法尺未预处理条件分支，合并识别 TryForwardObject / RouteLookupForTest / ForwardObjectImpl。原尺 rc=1、missing=1 added=1 原样保留。按 advisor `/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_484_implement_r5654131619-20260913T152359Z.md`，仅将 RouteLookupForTest 宏块在内存中逆搬移后复跑原尺，差集为空；另附每个搬移定义的 body SHA 和一字变更阳性对照。只证明文本守恒，不作函数语义推断。
