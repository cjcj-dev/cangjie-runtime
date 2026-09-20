LANE=sym_cangjie_runtime_736_implement_r5749368814
冻结 main=0189f2e0151506709f4ea9f2100956402f5c0e28 (rev-parse rc=0)。
源码证伪任务前提：testOldMarkStarted 已无产品声明/调用，native_root_current 已迁产品 mark_start/concurrent_mark；generation_cycle_context.cpp:143,172,211 仍引用三个已删除回调 testYoungMarkStarted/testCyclePrepared/testOldMarkStarted。OHOS:89 仍引用 testOldMarkStarted。
原回调断言读取旧 DoEnumeration 完成后、DoTracing 前的发布栈；现 ZGC concurrent_mark 内部 roots+follow 连续执行，现有 AFTER MARKING STARTED breakpoint 位于 roots 之前，BEFORE MARKING COMPLETED 位于 follow 之后。不能原样保留发布栈断言又不新增测试钩子。拟改为真实 mark_start/concurrent_mark 后产品存活/槽状态断言；这改变具体断言但保留根扫描不变量。请裁定是否授权此迁移，或指定可保留原断言的 ZGC 同形观测点。
追加 P-H/P-I/P-J/P-K 装置范围会逐项给处置表；不触碰邻包 #717/#720 产品行为。请确认删除收据后仍存在产品槽/存活观测的条目应改观测而非整条删除。
