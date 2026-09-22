# 修改前的 producer → consumer 顺序
1. runtime/src/Heap/z/zRelocate.cpp:813 AllocateRelocationTarget 设置 gc_relocation。
2. runtime/src/Heap/z/zHeap.cpp:489 Heap::alloc_page 原样传 flags 到 TakeRegion。
3. runtime/src/Heap/z/zPageAllocator.cpp:962-963 成功分配后增加 counter 并调用 sample_allocation；门控应在这两个调用之前，失败返回之后。
4. runtime/src/Heap/z/zStat.cpp:643,675 sample_allocation 更新采样统计并触发 evaluate_rules。
5. runtime/src/Heap/z/zDirector.cpp:617 消费 stats() 作为 mutator_alloc_rate。
ZGC 对应：/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageAllocator.cpp:1412-1419，在成功页之后按 flags 排除搬迁。
