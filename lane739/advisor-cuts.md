LANE=sym_cangjie_runtime_739_implement_r5747765432
young 持锁按裁定单独提交 f291daee3（先前产品队列中间态 daf57a2c4）。
断线规划：本次 notify_out_of_memory/restart_gc 是迁入的新调用，旧 CompleteWave 已从 zPageAllocator.hpp 删除。因此“切 satisfy false 出队 / restart_gc”两刀必然落在候选新增行，entry_cut_check 对这两刀会判新行不合法。拟：
1. 补一刀基线真实入口 StallAllocation 中既有 `return satisfied;` → `return false;`，以真实产品容量成功测试验证 consumer 返回值进入断言，并单独跑 entry_cut_check，要求 rc0。
2. 按任务指定另外切 NotifyOutOfMemory 的 satisfy(false) 和 HandleAllocStallingForOld 的 RestartGC（及年轻代对应点）作为新机制故障臂，独立报告，明确不能冒称基线旧行。
3. 退出排空另外切 driver 退出 RAII 调用，恢复每周期 reset 作对照；不改变 ZFuture。
请确认新迁入两刀作为机制证据、原有返回出口一刀作为基线入口接线证据的组合，或给具体可满足契约的切点。
