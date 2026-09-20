待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。
坐标b074e0431e39f64674f9b2b73e9d74bc0f337e5e。实现前顺序表：
|producer|consumer|修改点/理由|
|---|---|---|
|zGeneration.cpp:744 process_non_strong_references → pause_verify:1149|VM_ZVerifyOld::do_operation:276 → AfterWeakProcessing → Objects → z_verify_possibly_weak_oop:208|按ZGC zVerify.cpp:178-199直接调用old/finalizable、old地址/young标记、Heap活性与ZGenerationYoung::is_remembered；不能把“不是young”作为“是old”|
|ZRemembered::is_remembered zRemembered.inline.hpp:24|ZGenerationYoung::is_remembered（补ZGC zGeneration.inline.hpp:166-168薄包装）→ verifier|保持ZGC函数分解，消费现有产品_remembered，不新增数据或测试钩子|
|真实GC breakpoint BEFORE MARKING COMPLETED / AFTER CONCURRENT REFERENCE PROCESSING STARTED|真实old mark_end / pause_verify|测试只在已有断点修改真实字段颜色/位图，再恢复产品相位，目标断言须来自消费者结果|
