LANE=sym_cangjie_runtime_607_implement_r5738304930
ROLE=implement
PROGRESS=WIP
# Finalizable 数组 partial 消费端常量 false：申请最小 hunk
当前候选3098f7cef7d18e42a8b40f66ca52a837bc4b9a63 最终矩阵发现原同批数组Finalizable变体失败。kkk2:/root/sym_cangjie_runtime_607_implement_r5738304930/green/p2ArrayFieldExercise/final.log rc=4，fields=510 expected=1041，array_full_and_range_visit_all_fields / array_range_target_reached / array_first_child_retained_in_domain / array_last_child_retained_in_domain 四断言FAIL；Strong数组 rc=0、Finalizable struct rc=0（struct直接迭代不拆partial）。不是超时或前置CHECK。
我方runtime/src/Heap/z/zMark.cpp:1192-1196 old partial入口将MarkBarrierOnOldOopField(nullptr,field,false)写死false，完整对象分路:1216正确用entry.finalizable()；producer FollowElements :1520已将finalizable写进partial entry。ZGC zMark.cpp:198-205按finalizable选择字段屏障，:265-269 follow_partial_array传承finalizable；:382-403 follow_object入口用entry.finalizable()。
申请本轮最小修复：FollowPartialReferences的lambda捕获[&entry]，false改entry.finalizable()，不改mark重组/终止或其它包；本包要求字段闭包/数组范围/Finalizable，因此是同机制已列承重面。已触及冻结共享zMark函数，按原单写者裁定先请授权。修复后重绑最终SHA、两构型/DIFF/固定ELF切刀；保留当前失败为定位证据，另在最终SO回退此hunk做精确红。
已完成：Strong/Finalizable slow各1真实旧mark-start输入；真实minor在oldmark暂停时未标记old target轴通过；五基础用例/完整Finalizable字段/struct数组通过；3098三臂差分0/0/0但不据此交DONE。
