LANE=sym_cangjie_runtime_759_implement_r5749175544
ROLE=implement
验收前提核对：冻结 fd811238 的 AllocationStall.ProductReturnedCapacityServesOnlyOneWaiter 包在 runtime/tests/gc_unit/test_allocation_stall_queue.cpp:96-390 的 #if defined(MRT_TESTABLE_INTERNALS)，标准 default/filler ELF 不注册。MediumBlockingFailureAttemptsCollection 是无宏门控、可在 default/filler 各N3运行；ShutdownWithQueuedCollection 也仅 testable。刚合入的0189测试改版仍保留相同门控（已git show实核）。候选 default --gtest_list_tests 证据 kkk2:/root/sym_cangjie_runtime_759_implement_r5749175544-finalgreen/default-registration.txt。不把过滤0项当PASS。
请求：default/filler 验收是否按可注册的 medium 各N3＋全套差分执行，stall/Shutdown 保持 testable N3？若必须把 stall 迁入default，要同步撤掉该用例对 testable 计数/mark observer 的依赖，属于测试形态改造（#627已改为breakpoint，但仍门控）。当前不擅改门控/断言。
另：最终候选 e6a6517c30334058011e0e64226bf7aefe8a6079 green testable 两主目标各3/3通过；Shutdown N3=[1,0,0]，唯一红为已登记 #747 discoveredExternObjects.empty（不是#759），按常备裁决2逐发记录不计通过。已查重不另开重复issue。
