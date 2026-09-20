LANE=sym_cangjie_runtime_471_implement_r5747758651
冻结 b6d62daa8f3557c8a9effbfa4709a4744e315497，rev-parse rc=0。
任务前提实核与删除边界请求：
1. zRelocate.cpp:1260-1262 DrainForwardFromRegions<Old> 已传 old.relocation_set() 给 ForwardTask；:1530-1532 无 worker 变体同样消费选集；zPageAllocator.inline.hpp:264 ExecuteForwardTask 是共用选集消费者。old 选择入口 :1279 确为空，但“独立搬迁选路”不能按不存在选集消费者理解。是否保留共用 worker，删除 ForwardFromSpace/ForwardFromRegions 外壳并将两代统一为 ZRelocate::relocate 实例入口？这会涉及 young 同一外壳，需确认边界。
2. zHeuristics.cpp:88-93 已含 medium + NUMA；Director zDirector.cpp:637 重复小页公式。建议 Director 改调用现有 ZHeuristics::relocation_headroom，不倒退已在主线的 NUMA。
3. zGeneration.cpp:1435-1439 实际为 selector.stats() 按年龄求和，并非逐页重加；与 ZGC zGeneration.cpp:720-738 同形。建议删 :692-707 旧逐页账，保留 selector 年龄汇总，补 garbage/allocated/softmax 消费。
请裁定上述精确范围。尚未改产品。
