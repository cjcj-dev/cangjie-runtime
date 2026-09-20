LANE=sym_cangjie_runtime_471_implement_r5747758651
补充证伪，须暂停本轮主要修法裁定。
基线 b6d62daa 实际链：ZGenerationOld::concurrent_process_non_strong_references -> PostTrace(zRelocationSet.cpp:85) -> ZRelocate::RefineFromSpace(zRelocate.cpp:125) -> RegionSpace::RefineFromSpace(RegionSpace.h:153) -> RegionManager::ExemptFromRegions(zRelocationSetSelector.cpp:280-283) -> old.select_relocation_set(false)。任务书“唯一 young 调用、old 不构造 selector”不成立。
新真实 runtime 测试 OldRelocationStatistics.FullCollectionPublishesLiveInput 在基线 SO sha256 c42702991b654b6f810d3fe06717f004c3159cca57eb133880526474c89bb485 通过 rc=0，live=8388624，minimum_live=8388608；基线 old 空相位函数已反汇编确认为 ret。
证据 kkk2:/root/sym_cangjie_runtime_471_implement_r5747758651-baseline/stats-v2-target.log；候选同值，但这是重复选集风险，不是通过证据。
建议继续形态修复，不关 issue：将旧 ExemptFromRegions 内的 select 真正搬到 old concurrent_select_relocation_set；删除 RefineFromSpace/ExemptFromRegions 选集外壳和按选集中未标记页面计 freed 的第二次账，PostTrace 中 reset 移到 concurrent_reset_relocation_set；mark quarantine release 保留在适当阶段且不改 #741。请裁定此实际调用链修正。
另 testable 基线 runner rc=123，test_cycle_ref_saferegion.cpp:51,53 直接访问私有 cycleRefWorkStack；已登记 new_issues。四臂要求 status=ran 前需要该范围外测试编译问题处置。请明确本轮是否允许最小修复此既有测试接口使用（不改断言）。
目前保留 WIP，不把新用例基线通过称作有效红臂。
