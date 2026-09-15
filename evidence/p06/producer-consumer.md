# 修改前 producer → consumer 顺序
- GCThread::Init zDriver.cpp:399 设置 ConcGCThreads → GCWorkers::WorkerLoop zWorkers.cpp:95 设置 worker_id → marking task work zMark.cpp:989/1446 → MarkEngine::FollowWork zMark.cpp:1832 → Drain/StealGlobalRound → MarkStripeStackList::Pop zMarkStack.cpp:91 → MarkingSMR::hazard_ptr/free_node。
- MarkDomain::PrepareWork zMark.cpp:1877 → EnsureWorkers（ZPerWorker 构造必须在 ConcGCThreads 初始化之后；随后 resize 不重分配存储）。
- MarkingSMR::free_node 生产 _freeing；hazard_ptr 生产 _hazard_ptr；扫描所有 worker 状态消费两者；free 在 worker 静止后迭代所有槽。
- ZGC 锚：zValue.inline.hpp:108-114；zMarkingSMR.cpp:29-112；zMarkStack.cpp:98-121。
