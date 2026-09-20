# 本轮断线顺序表（候选 1d1cf0af83d05e8509f0cc575ec524a1a831dc30）
待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。
按 004805Z advisor：候选新路由行，非原主线刀；本轮不再重做形态，仅补接线证据。

| 结果 | producer → consumer（runtime/src 下） | 刀与目标 |
|---|---|---|
| TLAB slice | CompilerCalls.cpp:207 MCC_NewObject → ObjectModel/MObject.cpp:14 Allocate → Heap/z/zObjectAllocator.cpp:354 Allocate → zThreadLocalAllocBuffer.cpp:130 allocate_new_tlab → zCollectedHeap.cpp:137 alloc_tlab → zHeap.cpp:543 alloc → zThreadLocalAllocBuffer.cpp:133 FillTLAB | producer 多分8 bytes；consumer descriptor 少8 bytes；真实 MCC 两次补充后的地址间距/descriptor 实际长度 |
| shared small | zHeap.cpp:543 alloc → zObjectAllocator.cpp:245 alloc_small_object → :239 alloc_object_in_shared_page → :186 alloc_object/:182 alloc_object_atomic | small consumer 多分8 bytes，同一真实 TLAB 两次补充地址不连续 |
| medium cached | zObjectAllocator.cpp:350 alloc → :247 alloc_medium_object → :234 alloc_object_in_medium_page → :213 fast_medium → :169 AllocateSharedPage → :128 Heap::alloc_page → zHeap.cpp:552 TakeRegion → zPageAllocator.cpp:832 ClaimAllocationLocked → :731 ClaimPageMemory → :262 claim_capacity_fast_medium | producer 去 fast flag；consumer alloc_page 丢 medium flags；MCC 返回对象所在实际页尺寸与 capacity 进入断言 |
| large | zObjectAllocator.cpp:350 alloc → :249 alloc_large_object → :228 alloc_page | pageSize 多一 granule；真实 MCC 返回页几何必须为 align_up(requested,granule)，第二对象独占另一页 |
| segmented | CompilerCalls 的 MCC_NewObjArray/MCC_NewArray8 → ObjectModel/MArray.inline.h:141 initialize → Heap/z/zObjArrayAllocator.cpp:87 MemorySet → :93 yield_for_safepoint → :108 RequestGC(test bridge) → :144 SetInvisibleObject(false) | producer 跳过 segmented；consumer 最终 invisible 位保留；返回数组 payload/可见性/实际GC sequence 进入断言 |
| allocation stall | Heap::alloc_page → TakeRegion → EnqueueLocked/StallAllocation → ScopedEnterSaferegion → request.Wait | 等待前不进 saferegion，真实 mutator 状态 safe=false；目标测试观察真实产品 Wait 结果 |
