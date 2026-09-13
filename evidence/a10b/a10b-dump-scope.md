LANE=sym_cangjie_runtime_494_implement_r5655389950
已完成中间提交 db159a907（ActiveCycle 删除、generation owns workers/stats、old body unlock/remap+relocate-start relock），继续修编译/测试迁移。
闭合阻碍：zDriver.cpp RunDriverLoop 仍需消费非 GC TaskQueue<GCExecutor>（唯一产品用途 RequestHeapDump）。调用来自 SignalManager.cpp:436、ExceptionManager.cpp:82、ProfilerAgentImpl.cpp:73。ZGC driver 阻塞 receive，heap dump 由 VM_HeapDumper/外层执行，不消费这条队列；因此不能简单改 receive 后让 dump 永远不执行，也不能新增轮询/唤醒机制。
申请裁定：这些无对应的 driver heap-dump task 路由与其调用是否允许本包删除（保留底层 Inspector 实现供其归属包按 VM operation 接回），还是需本包一并按 ZGC heapDumper.cpp 的调用层迁移？后一项超出先前“其余机制不动”的边界。期间继续 generation 消费者与测试迁移，不等答空转。
