# #700 LoadFc 单项退出失败定位（只读）

## 原始结果不能由诊断复跑覆盖

候选：379aae5a86ca。主线程报告三臂均 ran：default 候选独红 1 项，filler/testable 候选独红 0。此处不将后两臂结果外推给 default，不将后续构建替代本候选。

原始证据：kkk2:/root/diff_379aae5a86ca/unit-default/run.log：
- 3491 `[ RUN ] LoadFc.SwapOldValueHealthyTargetReturnsNormally`
- 3492 同名 `[ PASS ]`
- 3493 `[========] 1 tests: 1 passed, 0 failed`
- 3494 timeout 报进程信号退出
- 3495 同名 `[ FAIL ]`
- 3496 `isolated process incomplete rc=139`
- 5349 同名列入 INCOMPLETE。

这证明目标断言实际执行并通过，而测试进程未有效完成；default 该项仍失败。不能记为套件独红 0。原 default 日志末尾另含 29 个 INCOMPLETE，这是原全量结果，不代表本次修复新增了 29 项；差集以主线程绑定 sha 的 DIFF 为准。

## 调用与 fork 关系

`runtime/tests/gc_unit/test_loadfc.cpp:126` 是普通 GC_TEST，测试体无 fork；`LoadFcFixture` 内含唯一 `GcHeapFixture`，:128 构造后 :130 在栈上创建 RefField，:133 进入 CJ_MCC_AtomicSwapReference，:135 断言旧值等于 obj0。

`runtime/tests/gc_unit/gc_heap_fixture.hpp:268` 构造触发 Heap 单例；`runtime/src/Heap/z/zCollectedHeap.cpp:55` 构造即创建 driver/director/stat。fixture 析构在测试函数返回前释放映射。原日志 PASS 在测试函数返回后才输出，因此异常的可见时间窗晚于测试函数及其 fixture 析构，属于进程完成/并行线程阶段，不能归因为 Swap 结果断言。

`test_segmented_array_init.cpp:1152` 的内部 fork 问题与此用例不相同。不能把那个已定位的原因直接套到该 case。构造线程参与退出阶段是待证假设，尚无该次 PC/栈证据。

## 单次 GDB 尝试的阻塞

拟用原 ELF kkk2:/root/diff_379aae5a86ca/unit-default/cj_gc_unit 及原 SO 执行一次隔离 GDB，命令保存在 `.lane-parallel/loadfc-one.gdb`（远端同棒 debug 目录亦有）。实际在运行前的 sha256 检查即失败：原 default/build 树已不在，runtime 与 bounds SO 路径不存在；install 目录亦查不到 SO。GDB **没有启动**，没有新测试判词、PC 或 maps，不能宣称复现或恢复。

远端 kkk2:/root/diff_379aae5a86ca/default-so.sha256 保存原 runtime sha256：
`5e2bda7c804ac7694431480460d60afedece85c21b75f94756cce09caad974c8`。
原测试 RUNPATH 指向已删除的 default/build/runtime-staging/lib/x86_64_Release。已告知父线程；若无保留同哈希 SO，不能用正在构建的新产物冒充原故障的复现。

本轮未修改测试源码、断言或产品；结论为“原测试断言通过但进程退出异常，原因尚未闭环”，继续保留该原始失败。
