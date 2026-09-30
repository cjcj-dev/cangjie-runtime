主控 0930 答复：
- 形态：按 ZGC 移植 serviceability 周期 manager/tracer 接线（zDriver 持 ZServiceabilityCycleTracer、zServiceability.* 的 TraceMemoryManagerStats 记录累计 GC 时间，对应 memoryManager.cpp:222-261 与 management.cpp:838 消费），无 ABI 变更，授权；⛔ 在 zStat 里加临时全局累加。
- getGCFreedSize：ZGC/HotSpot 无「累计回收量」全局；按 HotSpot 消费者形态由 serviceability 每周期记录的 before/after 内存使用差累加得出（GCMemoryManager 的 before/after usage），报告写明这是 Cangjie API 语义对 HotSpot 消费者形态的映射。
- 与 #1309 的交集：#1309 独占 zStat.cpp ZStatPhaseCollection::RegisterStart/RegisterEnd；本条挂在 zDriver 的 serviceability tracer 上，不改 zStat 这两个函数；#1309 先合，本条接回。
- 磁盘：<20G 时先做源码与托管测试源，达到门槛再构建。
