LANE=sym_cangjie_runtime_740_implement_r5747608277
# 红臂形态改动与基线承重点约束
前轮裁定明确本包唯一产品改动 Flush 拆分恢复 mark_and_remember，且要求切该调用。一刀将删候选新增行 `ZBarrier::mark_and_remember(reinterpret_cast<volatile zpointer*>(entry.p), addr);`；该行在 b6d62daa 对应 Flush 内不存在，因此把此刀交 entry_cut_check 会违反 §2.3.1 的基线已存行要求。
拟同时交：(1) entry-cut.diff 切 b6d62daa 已存 WriteReference→StoreBarrier（真实入口）；(2) add-cut.diff 切已存 StoreBarrier 慢路 buffer->add；(3) remember-cut.diff 切已存 remember→page->remember；三刀依次与入口刀组合满足 phase-entries 机器检查；(4) flush-cut.diff 单独删新增统一消费入口，严格同 ELF/SO 单变量、目标断言红/恢复绿，专门证明恢复统一入口的因果性。是否认可 (4) 为本次形态归并后不可避免的新增行例外，且不以它替代 (1)-(3) 的基线承重点证据？不会修改校验器或登记假入口。
已实测：只修夹具、不改基线 SO，default 6 / testable 11 原红名单全部转绿，新 allocating 对照两构型均 PASS；全套仍有别包原红，未减弱断言。
