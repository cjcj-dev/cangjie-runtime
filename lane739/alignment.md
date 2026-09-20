待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。

基线 b6d62daa8f3557c8a9effbfa4709a4744e315497；候选行号交付时由 git grep 重新生成。
| ZGC 锚 | 不变量 / 分路 | 本轮对应 |
|---|---|---|
| zPageAllocator.cpp:426-440 | 请求构造时保存两代 seqnum | ZPageAllocation 构造；const youngSeqnum/oldSeqnum |
| zPageAllocator.cpp:1518-1542 | claim失败后一次 non_blocking 判断，入队者必等待 | RegionManager::ClaimCapacityOrStall；TakeRegion把host GC线程/allowSaferegion能力翻译为flags |
| zPageAllocator.cpp:1436-1465 | 异步 minor，再唯一 wait，返回前删除信号量守卫 | StallAllocation；ScopedEnterSaferegion 为host握手适配 |
| zPageAllocator.cpp:2164-2189 | 容量归还锁内 claim→dequeue→satisfy(true) | ReturnRetiredPageMemory/物理分配回退→SatisfyStalledAllocations |
| zPageAllocator.cpp:2295-2363 | young/old seqnum判断；失败只答复已见old请求；其余restart | HasAllocSeenYoung/Old、NotifyOutOfMemory、RestartGC、HandleAllocStallingForYoung/Old |
| zDriver.cpp:193-225,434,454-484 | GC driver代别分解，ack后处理stall，major young结束也处理 | ZDriverMinor/Major::HandleAllocStalls、ExecuteDriverRequest |
| zGeneration.cpp:538-576,:995 | young全程持driver锁；只有old scope解锁 | 删除young额外DriverUnlocker；old保留，advisor 20260920T051926Z |
| zAbort.cpp:30-35；zDriver.cpp:201-225 | 停止abort不可在下轮GC清除 | 删除每周期reset；initialize_gc一次reset为host生命周期适配 |
| ZGC无allocator shutdown排空 | Cangjie本地Fini可在native allocation等待时停止；返回失败不得遗留信号量等待 | driver退出RAII→StopStalledAllocations锁内关队列并逐项答复；advisor 20260920T050835Z明确允许，无超时/轮询/ZFuture改动 |

删除清单：AllocationStallQueue类和旧独立观测头；CompleteWave；CaptureWaveBoundary；lastSequence；gcInProgress；request.sequence；等待者同步GC循环；TakeRegion第二次IsGcThread判定。

测试增删说明（WIP）：旧 OrdinaryAllocationCannotTakeSatisfiedPage、WaiterBlocksInSaferegion、CompletedWaveDoesNotFailLateWaiter、DequeueBeforeNotifyKeepsOneTerminalPerWaiter 和 GcDirector.AllocationStallSnapshotUsesOutstandingRequests，替换为真实 driver 的 ProductLateWaiterRequiresNextCollection 加强版（safe、队列、两代序号、一次终态）、ProductReturnedCapacityServesOnlyOneWaiter、ProductShutdownAnswersPendingWaiters。保留 OneFreeTreeUnitClaimsOnlyOneOfTwoWaiters 容量对照。P05Heuristics.ZPageAllocationIsStackRequest 保留断言，补构造前初始化两代的前提；不是豁免。run_standalone import检查迁到实际测试调用的Heap::alloc_page，不再要求测试越过入口直接调用StallAllocation。
