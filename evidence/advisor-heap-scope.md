LANE=sym_cangjie_runtime_465_implement_r5651041991
ROLE=implement
PROGRESS=WIP
A12a 迁移到 ZStatHeap 时核到基线 GcStats.cpp:159 将 oldLiveAtMarkEnd=usedAfter（周期末全堆已用），而 ZGC zStat.cpp:1788-1800 从 selector 各组 live 累加，非周期末 used。任务仅明确统计本体迁移/归属，未要求另做完整 heap stats 的口径改造。我方 young mark 已有 Generation.cpp:1205-1221 liveBytes，old 标记 live 可从 RegionManager::ExemptFromRegions 的既有遍历累计，但还需合并 pinned/large 范围；自行用 candidate-reclaimed 推 old live 不可证明口径。请裁定：本轮保留基线 oldLive 标量语义、移到 ZStat 并登记单独 issue，还是要求本轮一并实现真实 mark-end live 汇总？继续完成不依赖该答案的 registry/history 与两构型构建。
