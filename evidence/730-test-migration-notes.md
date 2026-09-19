# 测试迁移记账（WIP）

- TLABUsage：按 gc/shared/threadLocalAllocBuffer.inline.hpp:57-91 与 zHeap.cpp:144-160，把页粒度期望改为字节 TLAB 上限；空 TLAB 不再伪装 NullRegion。
- 本包旧 ThreadLocal 夹具：改以 RecentFull 或 From 标识真实页角色。移除 RegionRetirement.CompactInPlaceLeavesRegionOnAListACollectorWalks / StayYoungTransfersCompletedCompactTailFromThreadLocal：独占 TLAB 角色和对应转移已删除；保留 From→RecentFull 的两种完成顺序测试。
- AllocationStall 五项：移除替代 GC 回调，用真实 Heap::alloc_page(non_blocking) 检查容量独占，用产品 StallAllocation(false) 检查等待 saferegion；晚到/终态检查产品队列类。后两类不声称证明完整 GC OOM 驱动闭环。
- SegmentedArrayInit：删除返回人工指定地址的 allocate 回调与 Publish/Yield/Withdraw/RootVisit/RootPhase 回调。数组由 MCC_NewObjArray/MCC_NewArray8 获取；已登记 MRT_GC_UNIT_MANAGED_SEGMENTED 在真实清零窗口 RequestGC，返回后断言长度、清零、根撤销及 GC sequence。
- 旧 inactive/dirty/released/garbage 八条分支来自被 #727 合并的旧内存供给机制；由 mapped cache + 实际产品分配用例替代，不再从测试给产品返回地址。
- MarkAllocation：zRelocate.cpp:868-874 的目标页 reset/clone 可使存活页在新归属代仍为 allocating，旧 !IsAllocating 判据不适用；改查 survivor 年龄与当前归属序号，同时保留对象字段真实存活断言。
- 两个 ZGeneration 回调按 advisor 20260919T233036Z 留给 #736；本包消费者已删除。testPinnedPageAcquired 已删除。
- P1Mark.PinnedPagePublicationAcrossMarkStart 的旧中途拿页回调被删除；其跨获取窗口的精确调度验证尚待新证据，不能以其他 pinned 用例的绿冒充覆盖。

- 分段脚本 GC-window 守卫预演：真实 testable 日志中运行时日志与窗口行可共处一行（validation/unit-testable.log:6840、6978）。去掉行首锚，保留 mode/root/fields 结果边界，避免拦住已执行窗口的正常输入。default 构型走独立 construct 臂；full/young 必须检测到实际窗口。
