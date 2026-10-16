LANE=sym_cangjie_runtime_1322_implement_r5896137675
#1322 前提核查：ZGC 的累计管理GC时间不是 ZStatCycle::at_end / ZStatPhaseCollection::register_end 写入。
证据：/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDriver.cpp:173,389 持 ZServiceabilityCycleTracer；zServiceability.cpp:202-214 的 TraceMemoryManagerStats recordAccumulatedGCTime=true；services/memoryManager.cpp:222-227 start，:252-261 stop；memoryManager.hpp:161 gc_time_ms 返回 _accumulated_timer；management.cpp:838 消费。
我方 runtime/src/Heap/z/zServiceability.hpp:26-29 为空类，driver scopes 无 tracer。直接在 zStatCycle 或 ZStatPhaseCollection 累加两个原子虽可修恒0，但不满足同函数分解/数据形态硬约束。ZStatHeap at_relocate_end（ZGC zStat.cpp:1818-1842）只有 reclaimed快照与衰减序列，没有累计回收量全局。
请裁决本包是否移植最小 serviceability cycle manager/tracer 接线（涉及 zDriver.cpp / zServiceability.* / zHeap.*，无ABI变更），并明确 Cangjie getGCFreedSize 的累计值应采用 ZStatHeap 哪一消费者形态。按要求先问，不加临时全局累加；继续准备托管源程序和现有SDK身份核查。
