# #700 结构折叠后红臂源行追溯判据请求
LANE=sym_cangjie_runtime_700_implement_r5723534433
ROLE=implement
PROGRESS=WIP

冻结基线8da2396b88608f0ab3ddbc65ddf7b05a414abc0c。当前已删HeapGcState/collectorImpl/GetCollector、两旧目录；Heap值持page allocator/两代/crossVM，ZCollectedHeap构造起driver/director/stat并等待runtime完成。构建两构型0，398d880bf三臂ran且CAND-ONLY=0/0/0。尚不送审。

直接红臂的严格entry_cut_check在本次要求的重构后产生路径/owner文本矛盾（不是测试没走真实产品）：
1. producer现zCrossVM.cpp中discoveredExternObjects[exportObj].push_back(object); 原文逐字在冻结基线runtime/src/Heap/z/zMark.cpp:1182，git grep rc0。
2. consumer现zRelocationSet.cpp PostTrace内Heap::GetHeap().cross_vm().PrepareCycleRef(); 原文基线同file:67为PrepareCycleRef();，方法实体现在跨类。PostTrace仍在PHASE_ENTRIES识别范围。
工具只git show base:同路径且拒本包新增位置，不支持跨文件/owner迁移。五个切点预演rc1，且未变HandleTraceRegions调用阳性rc0，证明检查器确实运行；后者不证明export因果，未冒充合法红臂。完整预演与JSON在本树.lane-parallel/red-entry-plan.md及red-entry/。

拟实际控制臂：产品producer删除真实push；consumer删除真实PrepareCycleRef调用。测试仍为test_young_weak.cpp的ValueRootCurrentization.MajorDriverPairsExportOwnersBeforeHandoff，来自产品SO+真实driver入口，before/after结果进入producerCarrier/handoffCurrent断言，计划保同一ELF、四独立SO、hash与目标断言痕迹。实际运行继续做，不等答复空转。

请求明确裁决：本包可否以冻结基线原路径逐字对应+现路径真实入口caller链+原checker拒绝JSON作为公开的重构lineage例外，保留原base不伪造rc0？或者请指定合法的具体校验/基线例外（绑定本lane最终Delivery SHA及该两切口），不要求恢复已删collector层或把无关切刀凑作阳性。不会修改共享checker/PHASE_ENTRIES来绕过。

补充：最新CONTINUE写“不push”，本续轮遵守，候选提交只在同分支本地；如原任务的同分支push仍应执行，请在裁决明确该冲突。原PR #701远端暂398之前的f49。
