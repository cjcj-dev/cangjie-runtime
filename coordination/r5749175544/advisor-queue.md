LANE=sym_cangjie_runtime_759_implement_r5749175544
ROLE=implement
承接 103900Z 答复，正在实现两代 safepoint 同步。发现既有 ZRelocate::queue() 是每代成员，但真实 relocate/retain/wait/worker 都走 RegionManager::relocateQueue 单共享队列：zRelocate.cpp:115,246-248,879；zPageAllocator.inline.hpp:271,276；zForwarding.cpp:99；zForwardingTable.cpp:30-32 的 generation_relocate_queue 无 generation 参数。
若照字面给两代 synchronize_relocation 调各自 ZRelocate::queue()->synchronize，同步的是闲置队列（无产品资格）；若同调共享队列又不满足两代形态。
必要实现提案：删除 RegionManager 的共享队列字段/无代 getter，将上述真实消费者按 forwarding->table_generation 或 relocationSet->generation 路由至既有每代 ZRelocate 队列；把 worker 循环从 SelectBeforeOrdinary/SynchronizePoll 改为 ZGC zRelocate.cpp:1193-1211 的 synchronize_poll→ordinary→leave，以便 safepoint 真等待 worker（既有小写 synchronize_poll/synchronize/desynchronize 实现在场但未接产品循环）。同步修改受该旧 API 影响的测试调用。此项是获准 safepoint 接线的必要依赖，但可能与 #757 relocation worker 问题相撞，请确认范围/是否转交。
不改队列其它行为、不借用新增测试导出。当前 WIP；已做中间态可编译，尚未提交非批准队列迁移。
