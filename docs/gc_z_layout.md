# D09：GC 文件布局搬移表

本轮返工的定义级对应、诊断退回与 advisor 例外见 [gc_z_layout_rework.md](gc_z_layout_rework.md)。完整 TSV 的 ZGC 文件职责锚不代表逐函数等价。


待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

旧坐标基于 `3439312e41310da5c961ffa2025642e19081142f`；新坐标为本文件所在候选提交。候选分支：`sym/484-implement-r5653028906`。

本包按 ZGC 文件职责搬移定义，保留原类型、函数体、条件编译及已有机制。混合函数整体归主要职责文件；旧诊断、适配器与开关由后续包处理。对应表表示布局职责。

本轮 A1/A2 定义纠正见 [gc_z_layout_verify_rework.md](gc_z_layout_verify_rework.md)。VerifyRoots.cpp、VerifyHeap.cpp、VerifyRememberedSet.cpp 的产品定义已归入 zVerify.cpp / zForwarding.cpp，不属于自有诊断删除清单；停顿测试钩子六个定义留原 Allocator 层。

## 关键函数对应

| 旧定义 | 新定义 | ZGC 函数锚 |
|---|---|---|
| `runtime/src/Heap/Allocator/ZAttachedArray.h:24` | `runtime/src/Heap/z/zAttachedArray.inline.hpp:11` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAttachedArray.inline.hpp:33` |
| `runtime/src/Heap/Allocator/ForwardingAllocator.h:62` | `runtime/src/Heap/z/zForwardingAllocator.inline.hpp:49` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwardingAllocator.inline.hpp:39` |
| `runtime/src/Heap/Collector/ZForwarding.h:488` | `runtime/src/Heap/z/zForwarding.cpp:90` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:51` |
| `runtime/src/Heap/Collector/ZForwarding.h:493` | `runtime/src/Heap/z/zForwarding.cpp:93` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:86` |
| `runtime/src/Heap/Collector/ZForwarding.h:497` | `runtime/src/Heap/z/zForwarding.cpp:98` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:134` |
| `runtime/src/Heap/Collector/MarkStripe.cpp:45` | `runtime/src/Heap/z/zMarkStack.cpp:30` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:33` |
| `runtime/src/Heap/Collector/MarkStripe.cpp:479` | `runtime/src/Heap/z/zMarkCache.inline.hpp:11` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkCache.inline.hpp:53` |
| `runtime/src/Heap/Collector/MarkStripe.cpp:511` | `runtime/src/Heap/z/zMarkContext.inline.hpp:9` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkContext.inline.hpp:29` |
| `runtime/src/Heap/Collector/MarkEngine.cpp:45` | `runtime/src/Heap/z/zMarkTerminate.inline.hpp:39` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkTerminate.inline.hpp:66` |
| `runtime/src/Heap/Allocator/zPageAllocator.cpp:885` | `runtime/src/Heap/z/zPageAllocator.cpp:781` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageAllocator.cpp:1401` |
| `runtime/src/Heap/Barrier/StoreBarrierBuffer.cpp:72` | `runtime/src/Heap/z/zStoreBarrierBuffer.inline.hpp:10` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStoreBarrierBuffer.inline.hpp:37` |
| `runtime/src/Heap/Allocator/ZGranuleMap.h:292` | `runtime/src/Heap/z/zPageTable.inline.hpp:20` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageTable.inline.hpp:78` |
| `runtime/src/Heap/Allocator/ZGranuleMap.h:119` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp:80` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zIndexDistributor.inline.hpp:335` |
| `runtime/src/Common/ColourMask.h:204` | `runtime/src/Heap/z/zAddress.inline.hpp:10` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAddress.cpp:78` |

## 全部文件对应

候选 `Heap/z/` 共 128 个普通文件；每个均有同名 ZGC 文件。ZGC 顶层实读为 234 个普通文件及 2 个目录（236 个目录项）；参考测试目录有 13 个 `test_z*.cpp`。

| 冻结基线来源（runtime/src 下） | 新文件 | ZGC 文件锚 |
|---|---|---|
| `Heap/Collector/ZAbort.hpp:16` | `runtime/src/Heap/z/zAbort.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAbort.hpp:30` |
| `Common/BaseObject.cpp:226` | `runtime/src/Heap/z/zAddress.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAddress.cpp:65` |
| `Common/ColourMask.h:25` · `Common/ColourTypes.h:28` | `runtime/src/Heap/z/zAddress.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAddress.hpp:38` |
| `Common/ColourMask.h:204` · `Common/ColourPredicates.h:35` · `Common/ColourTypes.h:44` | `runtime/src/Heap/z/zAddress.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAddress.inline.hpp:49` |
| `Heap/Allocator/MemMap.cpp:361` | `runtime/src/Heap/z/zAddressSpaceLimit.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAddressSpaceLimit.cpp:33` |
| `Heap/Allocator/MemMap.h:34` | `runtime/src/Heap/z/zAddressSpaceLimit.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAddressSpaceLimit.hpp:30` |
| `Heap/Allocator/ZAttachedArray.h:16` | `runtime/src/Heap/z/zAttachedArray.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAttachedArray.hpp:29` |
| `Heap/Allocator/ZAttachedArray.h:24` | `runtime/src/Heap/z/zAttachedArray.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAttachedArray.inline.hpp:32` |
| `Heap/Barrier/Barrier.cpp:37` | `runtime/src/Heap/z/zBarrier.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zBarrier.cpp:36` |
| `Heap/Barrier/Barrier.h:25` | `runtime/src/Heap/z/zBarrier.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zBarrier.hpp:72` |
| `Heap/Barrier/Barrier.inline.h:17` | `runtime/src/Heap/z/zBarrier.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zBarrier.inline.hpp:41` |
| `Heap/Allocator/zPage.hpp:69` | `runtime/src/Heap/z/zBitField.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zBitField.hpp:59` |
| `Heap/Collector/Collector.cpp:39` | `runtime/src/Heap/z/zCollectedHeap.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zCollectedHeap.cpp:58` |
| `Heap/Collector/Collector.h:487` · `StackManager.cpp:97` | `runtime/src/Heap/z/zCollectedHeap.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zCollectedHeap.hpp:35` |
| `Heap/Collector/CollectorResources.cpp:274` | `runtime/src/Heap/z/zDirector.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDirector.cpp:37` |
| `Heap/Collector/GcTrigger.h:137` · `Heap/Collector/GcTriggerFlags.h:13` | `runtime/src/Heap/z/zDirector.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDirector.hpp:30` |
| `Heap/Collector/CollectorResources.cpp:49` | `runtime/src/Heap/z/zDriver.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDriver.cpp:39` |
| `Heap/Collector/CollectorResources.h:42` | `runtime/src/Heap/z/zDriver.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDriver.hpp:43` |
| `Heap/Collector/DriverPort.cpp:13` | `runtime/src/Heap/z/zDriverPort.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDriverPort.cpp:30` |
| `Heap/Collector/DriverPort.h:36` | `runtime/src/Heap/z/zDriverPort.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDriverPort.hpp:31` |
| `Heap/Collector/ZForwarding.h:488` · `Heap/Collector/ZForwardingLife.cpp:25` | `runtime/src/Heap/z/zForwarding.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:51` |
| `Heap/Collector/ZForwarding.h:71` · `Heap/Collector/ZForwardingLife.h:62` | `runtime/src/Heap/z/zForwarding.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.hpp:37` |
| `Heap/Collector/ZForwarding.h:489` | `runtime/src/Heap/z/zForwarding.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:43` |
| `Heap/Allocator/ForwardingAllocator.h:24` | `runtime/src/Heap/z/zForwardingAllocator.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwardingAllocator.cpp:27` |
| `Heap/Allocator/ForwardingAllocator.h:53` | `runtime/src/Heap/z/zForwardingAllocator.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwardingAllocator.hpp:30` |
| `Heap/Allocator/ForwardingAllocator.h:30` | `runtime/src/Heap/z/zForwardingAllocator.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwardingAllocator.inline.hpp:31` |
| `Heap/Allocator/ForwardingEntry.h:34` | `runtime/src/Heap/z/zForwardingEntry.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwardingEntry.hpp:49` |
| `Heap/Allocator/zForwardingTable.hpp:34` | `runtime/src/Heap/z/zForwardingTable.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwardingTable.hpp:30` |
| `Heap/Collector/Collector.cpp:32` · `Heap/Collector/Generation.cpp:56` · `Heap/Collector/TracingCollector.cpp:735` · `ObjectManager.h:26` | `runtime/src/Heap/z/zGeneration.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zGeneration.cpp:69` |
| `Heap/Collector/Collector.h:62` | `runtime/src/Heap/z/zGeneration.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zGeneration.hpp:42` |
| `Common/ColourMask.h:56` · `Heap/Collector/LiveInfo.h:30` | `runtime/src/Heap/z/zGenerationId.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zGenerationId.hpp:29` |
| `Heap/Collector/Collector.h:29` · `Heap/Collector/MarkPartialArray.h:39` · `Heap/Collector/MarkStripe.cpp:23` | `runtime/src/Heap/z/zGlobals.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zGlobals.hpp:32` |
| `Heap/Allocator/ZGranuleMap.h:145` | `runtime/src/Heap/z/zGranuleMap.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zGranuleMap.hpp:31` |
| `Heap/Allocator/ZGranuleMap.h:211` | `runtime/src/Heap/z/zGranuleMap.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zGranuleMap.inline.hpp:37` |
| `Heap/Heap.cpp:79` | `runtime/src/Heap/z/zHeap.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zHeap.cpp:53` |
| `Heap/Heap.h:36` | `runtime/src/Heap/z/zHeap.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zHeap.hpp:38` |
| `Heap/Collector/HeapIterator.cpp:12` | `runtime/src/Heap/z/zHeapIterator.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zHeapIterator.cpp:42` |
| `Heap/Collector/HeapIterator.h:20` | `runtime/src/Heap/z/zHeapIterator.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zHeapIterator.hpp:35` |
| `Heap/Allocator/ZGranuleMap.h:34` | `runtime/src/Heap/z/zIndexDistributor.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zIndexDistributor.hpp:29` |
| `Heap/Allocator/ZGranuleMap.h:45` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zIndexDistributor.inline.hpp:37` |
| `Heap/Allocator/RegionList.h:17` | `runtime/src/Heap/z/zList.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zList.hpp:31` |
| `Heap/Allocator/zPageAllocator.cpp:108` | `runtime/src/Heap/z/zList.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zList.inline.hpp:32` |
| `Heap/Collector/LiveInfo.h:43` | `runtime/src/Heap/z/zLiveMap.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.hpp:33` |
| `Heap/Allocator/CartesianTree.cpp:16` | `runtime/src/Heap/z/zMappedCache.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMappedCache.cpp:36` |
| `Heap/Allocator/CartesianTree.h:55` | `runtime/src/Heap/z/zMappedCache.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMappedCache.hpp:35` |
| `Heap/Collector/Mark.cpp:73` · `Heap/Collector/MarkEngine.cpp:99` · `Heap/Collector/MarkPartialArray.cpp:46` · `Heap/Collector/MarkingStacks.cpp:14` · `Heap/Collector/TracingCollector.cpp:303` | `runtime/src/Heap/z/zMark.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.cpp:75` |
| `Heap/Collector/MarkEngine.h:71` · `Heap/Collector/MarkStripe.h:145` · `Heap/Collector/MarkingStacks.h:8` · `Heap/Collector/TracingCollector.h:79` | `runtime/src/Heap/z/zMark.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.hpp:36` |
| `Heap/Collector/MarkStripe.cpp:471` | `runtime/src/Heap/z/zMarkCache.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkCache.cpp:28` |
| `Heap/Collector/MarkStripe.h:191` | `runtime/src/Heap/z/zMarkCache.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkCache.hpp:30` |
| `Heap/Collector/MarkStripe.cpp:479` | `runtime/src/Heap/z/zMarkCache.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkCache.inline.hpp:31` |
| `Heap/Collector/MarkStripe.h:215` | `runtime/src/Heap/z/zMarkContext.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkContext.hpp:30` |
| `Heap/Collector/MarkStripe.cpp:511` | `runtime/src/Heap/z/zMarkContext.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkContext.inline.hpp:29` |
| `Heap/Collector/MarkStripe.cpp:25` | `runtime/src/Heap/z/zMarkStack.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:33` |
| `Heap/Collector/MarkEngine.h:75` · `Heap/Collector/MarkStripe.h:33` | `runtime/src/Heap/z/zMarkStack.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.hpp:33` |
| `Heap/Collector/MarkStripe.cpp:81` | `runtime/src/Heap/z/zMarkStack.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp:33` |
| `Heap/Collector/MarkStackEntry.h:53` | `runtime/src/Heap/z/zMarkStackEntry.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStackEntry.hpp:74` |
| `Heap/Collector/MarkEngine.h:23` | `runtime/src/Heap/z/zMarkTerminate.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkTerminate.hpp:31` |
| `Heap/Collector/MarkEngine.cpp:17` | `runtime/src/Heap/z/zMarkTerminate.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkTerminate.inline.hpp:36` |
| `Heap/Collector/MarkStripe.cpp:104` | `runtime/src/Heap/z/zMarkingSMR.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkingSMR.cpp:29` |
| `Heap/Collector/MarkStripe.h:71` | `runtime/src/Heap/z/zMarkingSMR.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkingSMR.hpp:32` |
| `Heap/Collector/GcTrigger.h:27` | `runtime/src/Heap/z/zMetronome.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMetronome.hpp:30` |
| `Heap/Allocator/MemMap.cpp:393` | `runtime/src/Heap/z/zNUMA.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zNUMA.cpp:29` |
| `Heap/Allocator/MemMap.h:33` | `runtime/src/Heap/z/zNUMA.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zNUMA.hpp:31` |
| `Heap/Allocator/RegionSpace.cpp:65` · `Heap/Allocator/zObjectAllocator.cpp:61` | `runtime/src/Heap/z/zObjectAllocator.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zObjectAllocator.cpp:40` |
| `Heap/Allocator/zObjectAllocator.hpp:31` | `runtime/src/Heap/z/zObjectAllocator.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zObjectAllocator.hpp:35` |
| `Heap/Allocator/AllocBuffer.h:288` · `Heap/Allocator/zPage.cpp:80` · `Heap/Collector/LiveInfo.cpp:16` | `runtime/src/Heap/z/zPage.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.cpp:33` |
| `Heap/Allocator/zPage.hpp:158` | `runtime/src/Heap/z/zPage.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.hpp:36` |
| `Heap/Allocator/zPage.inline.hpp:15` | `runtime/src/Heap/z/zPage.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:41` |
| `Heap/Allocator/PageAge.h:40` | `runtime/src/Heap/z/zPageAge.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageAge.hpp:30` |
| `Heap/Allocator/PageAge.h:87` | `runtime/src/Heap/z/zPageAge.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageAge.inline.hpp:32` |
| `Heap/Allocator/RegionSpace.cpp:106` · `Heap/Allocator/zPageAllocator.cpp:71` | `runtime/src/Heap/z/zPageAllocator.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageAllocator.cpp:68` |
| `Heap/Allocator/AllocationStallQueue.h:45` · `Heap/Allocator/FreeRegionManager.h:30` · `Heap/Allocator/zPageAllocator.hpp:97` | `runtime/src/Heap/z/zPageAllocator.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageAllocator.hpp:45` |
| `Heap/Allocator/zPageAllocator.inline.hpp:15` · `Heap/Collector/zRelocate.hpp:20` | `runtime/src/Heap/z/zPageAllocator.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageAllocator.inline.hpp:29` |
| `Heap/Allocator/ZGranuleMap.h:285` | `runtime/src/Heap/z/zPageTable.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageTable.hpp:32` |
| `Heap/Allocator/ZGranuleMap.h:288` | `runtime/src/Heap/z/zPageTable.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageTable.inline.hpp:36` |
| `Heap/Allocator/MemMap.cpp:476` | `runtime/src/Heap/z/zPhysicalMemoryManager.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPhysicalMemoryManager.cpp:48` |
| `Heap/Allocator/MemMap.h:78` | `runtime/src/Heap/z/zPhysicalMemoryManager.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPhysicalMemoryManager.hpp:36` |
| `Heap/Allocator/RangeRegistry.h:15` | `runtime/src/Heap/z/zRangeRegistry.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRangeRegistry.hpp:33` |
| `Heap/Allocator/MemMap.cpp:49` · `Heap/Allocator/RangeRegistry.inline.h:25` | `runtime/src/Heap/z/zRangeRegistry.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRangeRegistry.inline.hpp:34` |
| `Heap/Collector/ReferenceProcessor.cpp:24` · `Heap/Collector/TracingCollector.cpp:467` | `runtime/src/Heap/z/zReferenceProcessor.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zReferenceProcessor.cpp:42` |
| `Heap/Collector/ReferenceProcessor.h:48` | `runtime/src/Heap/z/zReferenceProcessor.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zReferenceProcessor.hpp:31` |
| `Heap/Collector/RelocationRequestQueue.cpp:20` · `Heap/Collector/zRelocate.cpp:121` | `runtime/src/Heap/z/zRelocate.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRelocate.cpp:54` |
| `Heap/Collector/RelocationRequestQueue.h:34` · `Heap/Collector/zRelocate.hpp:12` | `runtime/src/Heap/z/zRelocate.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRelocate.hpp:33` |
| `Heap/Collector/zRelocationSet.cpp:64` | `runtime/src/Heap/z/zRelocationSet.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRelocationSet.cpp:39` |
| `Heap/Collector/zRelocationSet.inline.hpp:15` | `runtime/src/Heap/z/zRelocationSet.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRelocationSet.inline.hpp:31` |
| `Heap/Collector/zRelocationSetSelector.cpp:59` | `runtime/src/Heap/z/zRelocationSetSelector.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRelocationSetSelector.cpp:36` |
| `Heap/Collector/TenuringThreshold.h:33` · `Heap/Collector/zRelocationSetSelector.hpp:38` | `runtime/src/Heap/z/zRelocationSetSelector.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRelocationSetSelector.hpp:33` |
| `Heap/Collector/Remembered.cpp:75` | `runtime/src/Heap/z/zRemembered.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRemembered.cpp:41` |
| `Heap/Barrier/RememberedSet.cpp:39` | `runtime/src/Heap/z/zRememberedSet.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRememberedSet.cpp:34` |
| `Heap/Barrier/RememberedSet.h:48` | `runtime/src/Heap/z/zRememberedSet.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRememberedSet.hpp:30` |
| `Heap/Barrier/RememberedSet.cpp:72` | `runtime/src/Heap/z/zRememberedSet.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRememberedSet.inline.hpp:31` |
| `Heap/Collector/TracingCollector.cpp:137` | `runtime/src/Heap/z/zRootsIterator.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRootsIterator.cpp:37` |
| `Heap/Collector/TracingCollector.h:138` | `runtime/src/Heap/z/zRootsIterator.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRootsIterator.hpp:33` |
| `Heap/GcThreadPool.cpp:28` | `runtime/src/Heap/z/zRuntimeWorkers.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRuntimeWorkers.cpp:29` |
| `Heap/GcThreadPool.h:127` | `runtime/src/Heap/z/zRuntimeWorkers.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRuntimeWorkers.hpp:29` |
| `UnwindStack/StackWatermark.cpp:15` | `runtime/src/Heap/z/zStackWatermark.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStackWatermark.cpp:40` |
| `UnwindStack/StackWatermark.h:58` | `runtime/src/Heap/z/zStackWatermark.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStackWatermark.hpp:38` |
| `Base/ZStat.cpp:21` · `Heap/Collector/TracingCollector.cpp:1169` | `runtime/src/Heap/z/zStat.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:61` |
| `Base/ZStat.h:87` | `runtime/src/Heap/z/zStat.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.hpp:41` |
| `Heap/Barrier/StoreBarrierBuffer.cpp:25` | `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStoreBarrierBuffer.cpp:35` |
| `Heap/Barrier/StoreBarrierBuffer.h:36` | `runtime/src/Heap/z/zStoreBarrierBuffer.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStoreBarrierBuffer.hpp:33` |
| `Heap/Barrier/StoreBarrierBuffer.cpp:72` | `runtime/src/Heap/z/zStoreBarrierBuffer.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStoreBarrierBuffer.inline.hpp:33` |
| `Heap/Allocator/RegionSpace.cpp:258` | `runtime/src/Heap/z/zTLABUsage.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zTLABUsage.cpp:27` |
| `Heap/Allocator/AllocBuffer.h:26` | `runtime/src/Heap/z/zTLABUsage.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zTLABUsage.hpp:43` |
| `Heap/GcThreadPool.h:24` | `runtime/src/Heap/z/zTask.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zTask.hpp:30` |
| `Heap/Allocator/RegionSpace.cpp:37` | `runtime/src/Heap/z/zThreadLocalAllocBuffer.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zThreadLocalAllocBuffer.cpp:33` |
| `Heap/Allocator/AllocBuffer.h:60` · `Heap/Allocator/zPage.cpp:75` | `runtime/src/Heap/z/zThreadLocalAllocBuffer.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zThreadLocalAllocBuffer.hpp:31` |
| `Mutator/ThreadLocal.h:25` | `runtime/src/Heap/z/zThreadLocalData.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zThreadLocalData.hpp:35` |
| `Heap/Collector/Uncommitter.cpp:24` | `runtime/src/Heap/z/zUncommitter.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zUncommitter.cpp:41` |
| `Heap/Collector/Uncommitter.h:24` | `runtime/src/Heap/z/zUncommitter.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zUncommitter.hpp:31` |
| `Heap/Collector/MarkStripe.cpp:29` | `runtime/src/Heap/z/zUtils.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zUtils.inline.hpp:37` |
| `Heap/Verify/ZVerify.cpp:15` | `runtime/src/Heap/z/zVerify.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:116` |
| `Heap/Verify/ZVerify.h:8` | `runtime/src/Heap/z/zVerify.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.hpp:29` |
| `Heap/Allocator/MemMap.h:64` | `runtime/src/Heap/z/zVirtualMemory.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVirtualMemory.hpp:31` |
| `Heap/Allocator/RangeRegistry.inline.h:20` | `runtime/src/Heap/z/zVirtualMemory.inline.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVirtualMemory.inline.hpp:35` |
| `Heap/Allocator/MemMap.cpp:56` | `runtime/src/Heap/z/zVirtualMemoryManager.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVirtualMemoryManager.cpp:39` |
| `Heap/Allocator/MemMap.h:63` | `runtime/src/Heap/z/zVirtualMemoryManager.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVirtualMemoryManager.hpp:35` |
| `Heap/GcThreadPool.cpp:39` | `runtime/src/Heap/z/zWorkers.cpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zWorkers.cpp:33` |
| `Heap/GcThreadPool.h:21` | `runtime/src/Heap/z/zWorkers.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zWorkers.hpp:32` |
| `Heap/Collector/GcTrigger.h:56` · `Heap/Collector/zRelocationSetSelector.hpp:14` | `runtime/src/Heap/z/z_globals.hpp:1` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:27` |

逐函数旧/新锚和 body SHA256：[`gc_z_layout_functions.tsv`](gc_z_layout_functions.tsv)。词法扫描的 7,338 个函数体（含内联体、嵌套体）按名字、字节、条件上下文多重集匹配；隐式调用与重载解析不由词法调用清单单独判定。

校验补充来源：`Heap/Verify/VerifyRoots.cpp:13`、`VerifyHeap.cpp:18`、`VerifyRememberedSet.cpp:48` → `runtime/src/Heap/z/zVerify.cpp:71` 起；`VerifyRememberedSet.cpp:18 ZForwarding::verify` → `runtime/src/Heap/z/zForwarding.cpp:276`。精确逐定义锚见 gc_z_layout_verify_rework.md。

## hpp 声明新增

按 advisor 20260913T121748Z 裁定，原全树名字计数改为调用锚计数；类内定义拆出会增加声明。以下每项增加一个声明位置，函数体保留。

| 声明 | 位置 | 定义所在文件 |
|---|---|---|
| `static size_t object_size()` | `runtime/src/Heap/z/zAttachedArray.hpp:23` | `runtime/src/Heap/z/zAttachedArray.inline.hpp` |
| `static size_t array_size(size_t length)` | `runtime/src/Heap/z/zAttachedArray.hpp:25` | `runtime/src/Heap/z/zAttachedArray.inline.hpp` |
| `static bool allocation_size(size_t length, size_t* size)` | `runtime/src/Heap/z/zAttachedArray.hpp:28` | `runtime/src/Heap/z/zAttachedArray.inline.hpp` |
| `static void initialize(void* addr, size_t length)` | `runtime/src/Heap/z/zAttachedArray.hpp:30` | `runtime/src/Heap/z/zAttachedArray.inline.hpp` |
| `static void* alloc(size_t length)` | `runtime/src/Heap/z/zAttachedArray.hpp:32` | `runtime/src/Heap/z/zAttachedArray.inline.hpp` |
| `static void free(ObjectT* obj)` | `runtime/src/Heap/z/zAttachedArray.hpp:34` | `runtime/src/Heap/z/zAttachedArray.inline.hpp` |
| `size_t length() const` | `runtime/src/Heap/z/zAttachedArray.hpp:38` | `runtime/src/Heap/z/zAttachedArray.inline.hpp` |
| `ArrayT* operator()(const ObjectT* obj) const` | `runtime/src/Heap/z/zAttachedArray.hpp:40` | `runtime/src/Heap/z/zAttachedArray.inline.hpp` |
| `explicit ZAttachedArray(size_t length)` | `runtime/src/Heap/z/zAttachedArray.hpp:36` | `runtime/src/Heap/z/zAttachedArray.inline.hpp` |
| `static bool aligned_size(size_t size, size_t* aligned)` | `runtime/src/Heap/z/zForwardingAllocator.hpp:28` | `runtime/src/Heap/z/zForwardingAllocator.inline.hpp` |
| `static bool add_to_budget(size_t size, size_t* budget)` | `runtime/src/Heap/z/zForwardingAllocator.hpp:30` | `runtime/src/Heap/z/zForwardingAllocator.inline.hpp` |
| `bool valid() const` | `runtime/src/Heap/z/zForwardingAllocator.hpp:32` | `runtime/src/Heap/z/zForwardingAllocator.inline.hpp` |
| `size_t capacity() const` | `runtime/src/Heap/z/zForwardingAllocator.hpp:33` | `runtime/src/Heap/z/zForwardingAllocator.inline.hpp` |
| `size_t used() const` | `runtime/src/Heap/z/zForwardingAllocator.hpp:42` | `runtime/src/Heap/z/zForwardingAllocator.inline.hpp` |
| `void* allocate(size_t size)` | `runtime/src/Heap/z/zForwardingAllocator.hpp:44` | `runtime/src/Heap/z/zForwardingAllocator.inline.hpp` |
| `bool claim()` | `runtime/src/Heap/z/zForwarding.hpp:682` | `runtime/src/Heap/z/zForwarding.cpp` |
| `bool is_claimed() const` | `runtime/src/Heap/z/zForwarding.hpp:683` | `runtime/src/Heap/z/zForwarding.inline.hpp` |
| `bool in_place() const` | `runtime/src/Heap/z/zForwarding.hpp:684` | `runtime/src/Heap/z/zForwarding.inline.hpp` |
| `void set_in_place()` | `runtime/src/Heap/z/zForwarding.hpp:685` | `runtime/src/Heap/z/zForwarding.inline.hpp` |
| `bool retain_page()` | `runtime/src/Heap/z/zForwarding.hpp:686` | `runtime/src/Heap/z/zForwarding.cpp` |
| `void release_page()` | `runtime/src/Heap/z/zForwarding.hpp:687` | `runtime/src/Heap/z/zForwarding.cpp` |
| `void detach_page()` | `runtime/src/Heap/z/zForwarding.hpp:688` | `runtime/src/Heap/z/zForwarding.cpp` |
| `void mark_done()` | `runtime/src/Heap/z/zForwarding.hpp:689` | `runtime/src/Heap/z/zForwarding.cpp` |
| `bool is_done() const` | `runtime/src/Heap/z/zForwarding.hpp:690` | `runtime/src/Heap/z/zForwarding.cpp` |
| `void in_place_relocation_claim_page()` | `runtime/src/Heap/z/zForwarding.hpp:691` | `runtime/src/Heap/z/zForwarding.cpp` |
| `T get(zoffset offset) const` | `runtime/src/Heap/z/zGranuleMap.hpp:96` | `runtime/src/Heap/z/zGranuleMap.inline.hpp` |
| `void put(zoffset offset, T value)` | `runtime/src/Heap/z/zGranuleMap.hpp:98` | `runtime/src/Heap/z/zGranuleMap.inline.hpp` |
| `void put(zoffset offset, size_t size, T value)` | `runtime/src/Heap/z/zGranuleMap.hpp:100` | `runtime/src/Heap/z/zGranuleMap.inline.hpp` |
| `bool compare_exchange(zoffset offset, T& expected, T desired)` | `runtime/src/Heap/z/zGranuleMap.hpp:102` | `runtime/src/Heap/z/zGranuleMap.inline.hpp` |
| `T exchange(zoffset offset, T value)` | `runtime/src/Heap/z/zGranuleMap.hpp:104` | `runtime/src/Heap/z/zGranuleMap.inline.hpp` |
| `size_t granule() const` | `runtime/src/Heap/z/zGranuleMap.hpp:106` | `runtime/src/Heap/z/zGranuleMap.inline.hpp` |
| `size_t size() const` | `runtime/src/Heap/z/zGranuleMap.hpp:107` | `runtime/src/Heap/z/zGranuleMap.inline.hpp` |
| `MAddress base() const` | `runtime/src/Heap/z/zGranuleMap.hpp:108` | `runtime/src/Heap/z/zGranuleMap.inline.hpp` |
| `T at(size_t index) const` | `runtime/src/Heap/z/zGranuleMap.hpp:123` | `runtime/src/Heap/z/zGranuleMap.inline.hpp` |
| `size_t index_for_offset(zoffset offset) const` | `runtime/src/Heap/z/zGranuleMap.hpp:126` | `runtime/src/Heap/z/zGranuleMap.inline.hpp` |
| `explicit ZPageTableParallelIterator(const ZGranuleMap<T>& table)` | `runtime/src/Heap/z/zPageTable.hpp:18` | `runtime/src/Heap/z/zPageTable.inline.hpp` |
| `void do_pages(Function function)` | `runtime/src/Heap/z/zPageTable.hpp:21` | `runtime/src/Heap/z/zPageTable.inline.hpp` |
| `static size_t claim_level_index(const size_t* indices, size_t level)` | `runtime/src/Heap/z/zIndexDistributor.hpp:33` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp` |
| `static size_t claim_index(const size_t* indices, size_t level)` | `runtime/src/Heap/z/zIndexDistributor.hpp:35` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp` |
| `size_t level_segment_size(size_t level) const` | `runtime/src/Heap/z/zIndexDistributor.hpp:37` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp` |
| `static size_t get_count(size_t maxCount)` | `runtime/src/Heap/z/zIndexDistributor.hpp:55` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp` |
| `void claim_and_do(Function function, size_t* indices, size_t level)` | `runtime/src/Heap/z/zIndexDistributor.hpp:40` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp` |
| `void steal_and_do(Function function, size_t* indices, size_t level)` | `runtime/src/Heap/z/zIndexDistributor.hpp:43` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp` |
| `void do_indices(Function function)` | `runtime/src/Heap/z/zIndexDistributor.hpp:53` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp` |
| `explicit ZIndexDistributorClaimTree(size_t count)` | `runtime/src/Heap/z/zIndexDistributor.hpp:46` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp` |
| `explicit ForwardingAllocator(size_t capacity)` | `runtime/src/Heap/z/zForwardingAllocator.hpp:23` | `runtime/src/Heap/z/zForwardingAllocator.cpp` |
| `~ForwardingAllocator()` | `runtime/src/Heap/z/zForwardingAllocator.hpp:24` | `runtime/src/Heap/z/zForwardingAllocator.cpp` |

## 留在原位的内容

下表只登记 ZGC `gc/z` 无对应独立文件的余部；不据此断言功能不存在。匹配的定义已移出，混合函数与旧适配器保持原有函数体。后续包按删除/整合令处理。

| 原路径 | 原地内容 / 后续归属 |
|---|---|
| `runtime/src/Heap/Allocator/AllocBufferManager.h:19` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/AllocUtil.h:14` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/Allocator.cpp:19` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/Allocator.h:16` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/CartesianTree.cpp:14` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/CartesianTree.h:46` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/HeapFiller.cpp:16` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/HeapFiller.h:7` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/LocalDeque.h:24` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/RangeRegistry.cpp:1` | ZGC 无对应独立文件 · 测试构型的 out-of-line 物化桥；ZGC 无 zRangeRegistry.cpp，归 A12b |
| `runtime/src/Heap/Allocator/RegionList.h:14` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/RegionSpace.cpp:31` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/RegionSpace.h:27` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/RouteDestHold.cpp:9` | ZGC 无对应独立文件 · 旧搬移/持有关系/相位适配器；D06b / D07 |
| `runtime/src/Heap/Allocator/RouteDestHold.h:10` | ZGC 无对应独立文件 · 旧搬移/持有关系/相位适配器；D06b / D07 |
| `runtime/src/Heap/Allocator/RoutePublish.h:4` | ZGC 无对应独立文件 · 旧搬移/持有关系/相位适配器；D06b / D07 |
| `runtime/src/Heap/Allocator/RouteTicket.h:12` | ZGC 无对应独立文件 · 旧搬移/持有关系/相位适配器；D06b / D07 |
| `runtime/src/Heap/Allocator/SlotList.h:16` | ZGC 无对应独立文件 · 原分配前端、异步补充、容器/映射适配器；归 A12b / A06 |
| `runtime/src/Heap/Allocator/zForwardingTable.cpp:31` | ZGC 无对应独立文件 · forwarding arena 与安装状态；advisor 指定 D03b (#482) 重做持有关系 |
| `runtime/src/Heap/Collector/Collector.cpp:30` | ZGC 无对应独立文件 · 原诊断、语言 GC 前端/代理；归 A06 / D06b |
| `runtime/src/Heap/Collector/Collector.h:25` | ZGC 无对应独立文件 · 原诊断、语言 GC 前端/代理；归 A06 / D06b |
| `runtime/src/Heap/Collector/CollectorPlatform.h:10` | ZGC 无对应独立文件 · 原诊断、语言 GC 前端/代理；归 A06 / D06b |
| `runtime/src/Heap/Collector/CollectorProxy.cpp:10` | ZGC 无对应独立文件 · 原诊断、语言 GC 前端/代理；归 A06 / D06b |
| `runtime/src/Heap/Collector/CollectorProxy.h:16` | ZGC 无对应独立文件 · 原诊断、语言 GC 前端/代理；归 A06 / D06b |
| `runtime/src/Heap/Collector/CopyCollector.cpp:27` | ZGC 无对应独立文件 · 原诊断、语言 GC 前端/代理；归 A06 / D06b |
| `runtime/src/Heap/Collector/CopyCollector.h:15` | ZGC 无对应独立文件 · 原诊断、语言 GC 前端/代理；归 A06 / D06b |
| `runtime/src/Heap/Collector/FinalizerProcessor.cpp:24` | ZGC 无对应独立文件 · 仓颉 finalizer 处理器与语言适配；归引用处理包 |
| `runtime/src/Heap/Collector/FinalizerProcessor.h:22` | ZGC 无对应独立文件 · 仓颉 finalizer 处理器与语言适配；归引用处理包 |
| `runtime/src/Heap/Collector/GcInfos.h:29` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/GcRequest.cpp:14` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/GcRequest.h:17` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/GcStats.cpp:15` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/GcStats.h:21` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/LiveInfoArena.cpp:11` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/LiveInfoArena.h:19` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/ManagedObjectGate.h:10` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/MarkPartialArray.cpp:19` | ZGC 无对应独立文件 · 原开关、统计及命名适配声明；算法体已入 zMark.cpp，归 #228 / A06 |
| `runtime/src/Heap/Collector/MarkPartialArray.h:18` | ZGC 无对应独立文件 · 原开关、统计及命名适配声明；算法体已入 zMark.cpp，归 #228 / A06 |
| `runtime/src/Heap/Collector/PhaseColourContract.h:13` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/RemsetScanStats.h:10` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/StringDedup.cpp:11` | ZGC 无对应独立文件 · 共享去重处理器与 String ABI；ZGC gc/shared/stringdedup 层，归 L01 |
| `runtime/src/Heap/Collector/StringDedup.h:15` | ZGC 无对应独立文件 · 共享去重处理器与 String ABI；ZGC gc/shared/stringdedup 层，归 L01 |
| `runtime/src/Heap/Collector/TaskQueue.cpp:14` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/TaskQueue.h:23` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/Collector/TracingCollector.cpp:26` | ZGC 无对应独立文件 · 原诊断、语言 GC 前端/代理；归 A06 / D06b |
| `runtime/src/Heap/Collector/TruncatedSeq.h:13` | ZGC 无对应独立文件 · 旧统计/请求/任务/活性元数据适配器；归 A15 / A06 |
| `runtime/src/Heap/WCollector/RememberedHolderPolicy.h:8` | ZGC 无对应独立文件 · 旧搬移/持有关系/相位适配器；D06b / D07 |
| `runtime/src/Heap/WCollector/UntagRefFieldBreadcrumb.h:11` | ZGC 无对应独立文件 · 旧搬移/持有关系/相位适配器；D06b / D07 |
| `runtime/src/Heap/WCollector/WCollector.cpp:62` | ZGC 无对应独立文件 · 旧搬移/持有关系/相位适配器；D06b / D07 |
| `runtime/src/Heap/WCollector/WCollector.h:32` | ZGC 无对应独立文件 · 旧搬移/持有关系/相位适配器；D06b / D07 |
| `runtime/src/Heap/WCollector/WCollectorInternal.h:19` | ZGC 无对应独立文件 · 旧搬移/持有关系/相位适配器；D06b / D07 |
| `runtime/src/Heap/Verify/AllocPhaseDiag.h:24` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/CsetEmptyWho.cpp:28` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/CsetEmptyWho.h:12` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/DiagGate.cpp:16` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/DiagGate.h:35` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/FillerZeroDiag.cpp:9` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/FillerZeroDiag.h:7` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/GarbRegionDiag.cpp:17` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/GarbRegionDiag.h:13` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/HealCoverage.cpp:13` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/HealCoverage.h:32` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/HealPairDiag.cpp:10` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/HealPairDiag.h:9` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/HoleWhoDiag.cpp:13` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/HoleWhoDiag.h:7` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/InteriorEdgeClass.h:32` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/M0Correlation.cpp:26` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/M0Correlation.h:12` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/M0ExitDiagnostics.cpp:19` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/M0ExitDiagnostics.h:13` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/MinorGCALot.cpp:19` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/MinorGCALot.h:13` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/NwDropAudit.h:12` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/Stw2CurrentAudit.cpp:21` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/Stw2CurrentAudit.h:16` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/SurvNodeDiag.cpp:24` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/SurvNodeDiag.h:25` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/TraceClear.cpp:21` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/TraceClear.h:15` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/Zap.cpp:15` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/Zap.h:15` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/ZgcInvariants.cpp:22` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/ZgcInvariants.h:12` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/ZgcSelfHealDiag.cpp:18` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Heap/Verify/ZgcSelfHealDiag.h:32` | ZGC 无对应独立文件 · 自有诊断和观测；#228 / A06 |
| `runtime/src/Common/ColourEncoding.h:16` | ZGC 无对应独立文件 · 原编码校验器及地址范围适配；归 A06 |

混合定义示例：`TracingCollector` 的类声明归 zMark.hpp，其 generation/roots 方法分居各职责文件；pre/post 的独立 root 统计退回 TracingCollector.cpp，root iterator 留在 zRootsIterator.cpp。`RegionManager` 的 inline 调用模板保留在 zPageAllocator.inline.hpp；relocation queue 声明/实现归 zRelocate。`MemMap` 保留原类，其分区、NUMA、地址预算、物理提交定义分居对应文件。

## 删除路径与引用

仅删除已退休的源路径，机制不在本包删除。完整清单及 `git grep` 原文随报告证据交付；基线 grep 是阳性对照。剩余 `Heap/Allocator/Collector/WCollector` 引用全部指向上表原地余部。历史 negative patch 只更新目标路径，不宣称其旧 hunk 可应用。

## 测试路径

| 原路径 | 新路径 | ZGC 测试 |
|---|---|---|
| `runtime/tests/colour_predicates_unit.cpp` | `runtime/tests/test_zAddress.cpp` | `test/hotspot/gtest/gc/z/test_zAddress.cpp` |
| `runtime/tests/z_index_distributor_unit.cpp` | `runtime/tests/test_zIndexDistributor.cpp` | `test/hotspot/gtest/gc/z/test_zIndexDistributor.cpp` |
| `runtime/tests/gc_unit/test_z_list.cpp` | `runtime/tests/gc_unit/test_zList.cpp` | `test/hotspot/gtest/gc/z/test_zList.cpp` |
| `runtime/tests/gc_unit/test_forwarding_entries.cpp` | `runtime/tests/gc_unit/test_zForwarding.cpp` | `test/hotspot/gtest/gc/z/test_zForwarding.cpp` |
| `runtime/tests/gc_unit/test_region_bitmap.cpp` | `runtime/tests/gc_unit/test_zLiveMap.cpp` | `test/hotspot/gtest/gc/z/test_zLiveMap.cpp` |
| `runtime/tests/gc_unit/test_z_bit_field.cpp` | `runtime/tests/gc_unit/test_zBitField.cpp` | `test/hotspot/gtest/gc/z/test_zBitField.cpp` |
| `runtime/tests/gc_unit/test_page_age.cpp` | `runtime/tests/gc_unit/test_zPageAge.cpp` | `test/hotspot/gtest/gc/z/test_zPageAge.cpp` |

测试名/函数体保持；本轮未运行 gate、unit 或切刀。构建尝试及两构型产物身份见交付报告。`windows_x86_64_exports.def` 保持原字节，类型名/导出名未重命名。

## 独立后续项

cangjie-runtime#488：冻结基线的 cast 精确行号白名单已过期；本包保留原白名单，不扩大允许集合。

## 返工补充文件

| 文件 | ZGC 同名参考/原地归属 |
|---|---|
| `runtime/src/Heap/z/zAbort.cpp` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAbort.cpp` |
| `runtime/src/Heap/z/zAbort.inline.hpp` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAbort.inline.hpp` |
| `runtime/src/Heap/z/zForwardingTable.inline.hpp` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwardingTable.inline.hpp` |
| `runtime/src/Heap/z/zLiveMap.cpp` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.cpp` |
| `runtime/src/Heap/z/zLiveMap.inline.hpp` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp` |
| `runtime/src/Heap/z/zMark.inline.hpp` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.inline.hpp` |
| `runtime/src/Heap/z/zMetronome.cpp` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMetronome.cpp` |
| `runtime/src/Heap/z/zNUMA.inline.hpp` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zNUMA.inline.hpp` |
| `runtime/src/Heap/Allocator/RegionInfo.h` | ZGC 无独立对应；#228 / A06 原地保留 |
| `runtime/src/Heap/Allocator/RegionSpace.cpp` | ZGC 无独立对应；#228 / A06 原地保留 |
| `runtime/src/Heap/Allocator/zPage.cpp` | ZGC 无独立对应；#228 / A06 原地保留 |
| `runtime/src/Heap/Allocator/zPageAllocator.cpp` | ZGC 无独立对应；#228 / A06 原地保留 |
| `runtime/src/Heap/Collector/Generation.cpp` | ZGC 无独立对应；#228 / A06 原地保留 |
| `runtime/src/Heap/Collector/Mark.cpp` | ZGC 无独立对应；#228 / A06 原地保留 |
| `runtime/src/Heap/Collector/Remembered.cpp` | ZGC 无独立对应；#228 / A06 原地保留 |
| `runtime/src/Heap/Collector/TracingCollector.cpp` | ZGC 无独立对应；#228 / A06 原地保留 |
| `runtime/src/Heap/Collector/zRelocate.cpp` | ZGC 无独立对应；#228 / A06 原地保留 |
| `runtime/src/UnwindStack/StackWatermark.cpp` | ZGC 无独立对应；#228 / A06 原地保留 |

RecentFullAccounting 的声明原地保留于 `runtime/src/Heap/Allocator/RegionManager.h:9`；本轮不删除诊断或开关。
