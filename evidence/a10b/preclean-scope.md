LANE=sym_cangjie_runtime_494_implement_r5655389950
A10b 核心已落：双阻塞 receive、generation phase/seq/workers/stats 独立、old 主体放锁且 remap-young-roots/relocate-start 重取、ActiveCycle/全局完成状态/tag 状态删除；两构型此前已构建 rc=0，最终登记正在整理。
完整 driver 对照的范围边界：ZGC zDriver.cpp:282-325 should_preclean_young 与 :416-436 collect_young 对某些 major cause 先执行 major_full_preclean（promote all），再执行 major_full_roots；冻结我方 :348-365 的 major 前奏只有一次 combined roots young。任务书明确“major前奏同时建立两代起点已有代码保留”，此前裁定又要求其余标记/搬移/remset机制不动。本包保留 combined roots 前奏，但没有发明 promote-all 策略。
请明确 preclean/promote-all 分支是否属于本轮 A10b 三条不变量的必要范围；若是，需允许 generation young-type 与现有 tenuring/promote-all 消费接线；若是独立的原因策略/代际晋升问题，请明确归属或允许作为独立 issue 登记。当前保持 WIP，继续整理本包对应表、删除原文与最终构建。
