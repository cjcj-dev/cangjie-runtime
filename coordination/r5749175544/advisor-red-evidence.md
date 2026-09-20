LANE=sym_cangjie_runtime_759_implement_r5749175544
ROLE=implement
已按 104352Z 实现五机制+VM concurrent rendezvous 同串行入口，head=c2b8dc50224eeaa41466b04f546273597dda69e8。两构型 rc=0。冻结主线仍 fd811238（fetch+merge 已完成）。
取得因果红臂：冻结基线存在的 ZGenerationOld::concurrent_mark 调用 ZMark::DoEnumeration(workStack, foreignStack) 被切为原三段根处理（EnumAllCommonRoots / MergeMutatorRoots / EnumAllExportRoots），仅旁路新增 old-root STS 作用域。entry_cut_check rc=0，切刀在真实 concurrent_mark 入口。两臂均真实产品 SO、同候选 ELF、0-15 核域并行 N=3：green 两测各0,0,0；cut stall=1,0,0，medium=1,1,0。红日志签名另行实读；不把这个结果声称成确定性 3/3。
路径 kkk2:/root/sym_cangjie_runtime_759_implement_r5749175544-{green,cut}/targets/，ELF=/root/sym_cangjie_runtime_759_implement_r5749175544-queues/unit-testable/cj_gc_unit。已启动四臂 DIFF。
请求裁定：是否接受「既有真实容量耗尽/排队minor构造 + 基线入口刀 + N=3逐发如实记录」作为新承重面因果证据，而不另造 ConcurrentGCBreakpoints 案例？当前 breakpoint 只有 AFTER MARKING STARTED/BEFORE MARKING COMPLETED/AFTER CONCURRENT REFERENCE PROCESSING STARTED，不能停在 old-root barrier 内；两种单独 before/after 阶段都在 STS scope 外。要严格构造之前答复所述 old-root barrier 与 young mark-start 交错，需要增加产品内 checkpoint（常备裁决4禁新增钩子）或改变控制器，目前没有擅自加。若必须新构造，请明确允许的相位控制点；不通过重跑取绿/碰红。
当前保持 WIP；继续恢复臂、四臂差分、default/filler、OHOS 尝试及文档。
