LANE=sym_cangjie_runtime_730_implement_r5746042754
ROLE=implement
PROGRESS=WIP
# 请求裁决：#730 页缓存规格锚不在当前参考树
冻结 main 回读 rc=0：8cbac1ea8ef31dbaf5d49d3a1fe67300938ae74c。
任务书要求读取 zPageCache.{hpp,cpp} 并重做 small/medium/large 页缓存，但指定参考目录没有这两个文件。当前 ZGC zPageAllocator.cpp:764-780 为 ZPartition::claim_capacity_fast_medium，使用 ZMappedCache::remove_contiguous_power_of_2；当前我方 zMappedCache.hpp:132 已有对应接口，zPageAllocator.cpp:236 的 ClaimPageMemory 尚未消费 fast_medium。
请求明确：本条按当前 ZGC 的 ZMappedCache + claim_capacity_fast_medium 形态补全，不新增旧版 ZPageCache，是否正确？
另 TLAB 首字段 tlRegion 的编译器 ABI（zThreadLocalAllocBuffer.cpp:100）仍是页指针；共享页内 TLAB 应为独立 start/top/end，必须同步编译器分配发射。请提供本轮可读编译器坐标及跨仓修改归属；运行时不会用伪 ZPage 保留旧形态。
等待期间继续核查对象分配器函数分解与 flags 传递。
