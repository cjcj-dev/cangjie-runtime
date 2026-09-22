# 修改前的 producer → consumer 顺序
1. runtime/src/Heap/z/zRelocate.cpp:813 AllocateRelocationTarget 设置 gc_relocation。
2. runtime/src/Heap/z/zHeap.cpp:489 Heap::alloc_page 原样传 flags 到 TakeRegion。
3. runtime/src/Heap/z/zPageAllocator.cpp:962-963 成功分配后增加 counter 并调用 sample_allocation；门控应在这两个调用之前，失败返回之后。
4. runtime/src/Heap/z/zStat.cpp:643,675 sample_allocation 更新采样统计并触发 evaluate_rules。
5. runtime/src/Heap/z/zDirector.cpp:617 消费 stats() 作为 mutator_alloc_rate。
ZGC 对应：/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPageAllocator.cpp:1412-1419，在成功页之后按 flags 排除搬迁。

## 返工：初始化完成生产端（修改前登记）
- CangjieRuntime.cpp:114-116: Init 返回 → NotifyRuntimeInitialized 发布完成。
- concurrentGCThread.cpp:37-42: release 写入既有 completed；新增只读 acquire 查询，对应 reference/jdk/src/hotspot/share/runtime/init.cpp:241-243。
- zPageAllocator.cpp:963: 成功分配之后、两个统计消费者之前，合取初始化完成条件；对应 ZGC zPageAllocator.cpp:1414-1418。
- test_zstat.cpp 的初始化后用例通过 CJ_ScheduleManagerInit / MRT_CjRuntimeInit 建立真实完成状态；初始化前用例保留 standalone heap。
