LANE=sym_cangjie_runtime_607_implement_r5684610492
ROLE=implement
候选已 merge main 2677e74382846076984b0adf4e5df5e953b46dad（合并 b7094ee10）。P01 API在场，P02尚未合。
先读证发现本条独立Finalizable slow接入的相位前提不成立：runtime/src/Heap/z/zGeneration.cpp:748-761 ProcessOldNonStrongReferences CHECK old=MARK_COMPLETE，然后 DoResurrection:827-846 用MarkAndFollow(finalizable=true)进入RunMajorStripeMark，最终字段屏障。GenerationCycle::IsPhaseMark (zGeneration.inline.hpp:8-12)仅ENUM/TRACE/CLEAR，MarkObject:18 CHECK(IsPhaseMark)。按ZGC zBarrier.cpp:234-250新Finalizable slow应调用old.MarkObject<...,true>并保留during_any_mark CHECK，因此真实Finalizable字段会被更早相位CHECK拦住。
ZGC zReferenceProcessor.cpp:239-250是在discover_reference中立即mark_barrier_on_old_oop_field(referent_addr,true)，ZGeneration mark_object:118-122仍要求phase mark；其ReferenceProcessor discover在mark中消费。现C延迟到non-strong完成后。
本棒不放宽CHECK、不把Finalizable降Strong、不绕过GenerationCycle入口。请裁定该生产时机归属：是否授权本棒最小迁入mark阶段finalizable发现/字段follow，或由P3/根种子所有者接生产时机，并允许本轮如实保留范围缺口？同时继续完成Strong/remset/young轴的独立工作。
