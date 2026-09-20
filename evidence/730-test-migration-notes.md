# 测试迁移记账（#730返工补齐）

- TLABUsage：按 gc/shared/threadLocalAllocBuffer.inline.hpp:57-91 与 zHeap.cpp:144-160，把页粒度期望改为字节 TLAB 上限；空 TLAB 不再伪装 NullRegion。
- 本包旧 ThreadLocal 夹具：改以 RecentFull 或 From 标识真实页角色。移除 RegionRetirement.CompactInPlaceLeavesRegionOnAListACollectorWalks / StayYoungTransfersCompletedCompactTailFromThreadLocal：独占 TLAB 角色和对应转移已删除；保留 From→RecentFull 的两种完成顺序测试。
- AllocationStall 五项：移除替代 GC 回调，用真实 Heap::alloc_page(non_blocking) 检查容量独占，用产品 StallAllocation(false) 检查等待 saferegion；晚到/终态检查产品队列类。独立队列用例仅证明队列本体；返工新增 ProductLateWaiterRequiresNextCollection 通过两个真实 Heap::alloc_page 与既有 mark observer 构造跨GC边界，late切刀验证产品 CompleteWave 的边界消费。
- SegmentedArrayInit：删除返回人工指定地址的 allocate 回调与 Publish/Yield/Withdraw/RootVisit/RootPhase 回调。数组由 MCC_NewObjArray/MCC_NewArray8 获取；已登记 MRT_GC_UNIT_MANAGED_SEGMENTED 在真实清零窗口 RequestGC，返回后断言长度、清零、根撤销及 GC sequence。
- 旧 inactive/dirty/released/garbage 八条分支来自被 #727 合并的旧内存供给机制；由 mapped cache + 实际产品分配用例替代，不再从测试给产品返回地址。
- MarkAllocation：zRelocate.cpp:868-874 的目标页 reset/clone 可使存活页在新归属代仍为 allocating，旧 !IsAllocating 判据不适用；改查 survivor 年龄与当前归属序号，同时保留对象字段真实存活断言。
- 两个 ZGeneration 回调按 advisor 20260919T233036Z 留给 #736；本包消费者已删除。testPinnedPageAcquired 已删除。
- P1Mark.PinnedPagePublicationAcrossMarkStart 的旧中途拿页回调被删除；现由 pinned_publication_window.cpp + .gdb + run_pinned_publication_window.sh 恢复同一获取→mark-start→发布不变量；外部GDB在产品取页返回后调真实RequestGC，随后检查同一page的IsAllocating、下一对象地址与保活。切掉ResetPageSequence时仅current断言变假，ordering/shared/retained保持真（N=1，每构型）。

- 分段脚本 GC-window 守卫预演：真实 testable 日志中运行时日志与窗口行可共处一行（validation/unit-testable.log:6840、6978）。去掉行首锚，保留 mode/root/fields 结果边界，避免拦住已执行窗口的正常输入。default 构型走独立 construct 臂；full/young 必须检测到实际窗口。

- 四臂 DIFF 56d880754889 实测独红：ZPageGranule.MutatorAllocatesThreePageSizes 仍硬编码 MediumMin；按 zObjectAllocator.cpp:144 的冷请求 MediumMax 修正。可变缓存命中另由 FastMediumConsumesCachedActualSize 验证。
- managed 默认臂明确为 construct；testable 门显式传 both，强制 full/young GC 窗口。原“同一次 checksum 返回 0 就叫 GC 通过”的判据拆开，没有豁免 GC 窗口失败。

- SharedSmallPage.MigrationUsesCurrentCPU：删除提前返回，按 ZGC zHeuristics.cpp:69-74 以真实容量预算触发per-CPU；新增 SmallHeapUsesSharedSlotZero 覆盖另一输入。错误slot0切刀仅迁移目标变红，fallback与atomic bounds控制保持绿。
