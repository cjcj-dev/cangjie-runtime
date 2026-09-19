LANE=sym_cangjie_runtime_607_implement_r5738304930
ROLE=implement
PROGRESS=WIP
# R2 真实晋升暴露登记接缝，申请最小范围裁定
候选2989c86fa3c1，按审查R2删除AgePinnedToOld，改为真实GC_REASON_USER promote-all，export roots保活，之后原store/young GC。kkk2:/root/sym_cangjie_runtime_607_implement_r5738304930/probe/p2FieldBarrierExercise/run.log：real_holder_is_old PASS、real_child_is_young PASS、independent_young_root_control PASS；remset_retains_child_first_cycle / sentinel FAIL，rc=2。
静态定位：runtime/src/Heap/z/zRelocate.cpp:2348 reset(toAge)，2352 reset_livemap，2354 promoted.append；没有页表replace。zPageTable.cpp:63-71 的replace会register_with_remset，而原测试AgePinnedToOld曾手工补register_with_remset，掩盖真实生产端缺口。ZGC zRelocate.cpp:1357-1361 flip_promote → zGeneration.cpp:941-942 page_table.replace → zPageTable.cpp:68-79 register_with_remset。
请求允许本轮最小补：在ZFlipAgePagesTask promotion分路 reset_livemap 后调用 Heap::page_table()->replace(prev,newPage)（按本树API校准），再append；保持现有页描述符原地reset基础设施，不改pinned分配/forwarding生命周期。或由指定单一owner提供这一hunk后本棒消费。不恢复测试手工登记。
R1独立进展：原testYoungMarkStarted在old mark_start之前（VM_ZMarkStartYoungAndOld:220-221），改既有testMarkStartState(old,Complete)后强/Finalizable slow各1，位图/发布/不heal及fast控制全部PASS，无颜色/phase注入；末尾Finalizable sentinel分类断言待在mark结束前取值核对（当前follow=1，结束后分类FAIL）。继续独立整理固定ELF切刀设施，报告保持WIP。
