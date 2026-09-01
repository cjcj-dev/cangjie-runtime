PROGRESS=WIP · LANE=impl_gcunit3_r3 · ROLE=implement

任务书要求选择 T11 出路 ⓐ：保留 SIGABRT/root 不变，并把 S0/S1、rootFix、active/retired 分类证据跨 fork 搬回父进程。

实核发现两个前提冲突：

1. 当前 tip `8ec281948aa781947a5b46bcbd9db124d7ec01da` 下，产品代码对 `M0ExitDiagnostics::Exit::RootFix` 为 0 个调用点；`git grep -n 'M0ExitDiagnostics::Note' -- runtime/src` 只命中 `Barrier.cpp:1138,1142` 的 `Exit::ReadBarrier`。因此跨 fork 只能搬到零增量，无法恢复旧 `rootFix/S0/S1` 契约。
2. 当前 `RootFixRuntimeEnumerationClassifiesNoCopyAsS0` 为触发 SIGABRT 主动执行 `fx.heap.obj0->SetStateCode(ObjectState::FORWARDED)`（`runtime/tests/gc_unit/test_m0_exit.cpp:668`）。分类器 `M0ExitDiagnostics.cpp:151-160` 把 `target->IsForwarded()` 计作 `copyPublished`，所以即使恢复 RootFix 发射，该 fixture 也会归 S1，不会归 S0。

请求裁决最小返工边界：

- 是否允许在 `WCollector::FixMinorEvacuatedSlot(RootSlot&)` 的真实 fail-closed 前恢复 `M0ExitDiagnostics::Note(Exit::RootFix, ...)` 产品发射，再用共享内存/SIGABRT handler 跨 fork 搬运快照？
- NoCopyAsS0 是否应构造一个“无 active/retired/copyPublished 但仍走 fail-closed”的真实产品态；若当前 fail-closed 只对 non-Usable（FORWARDED/zero-header）成立，S0 与 SIGABRT 是否结构冲突，应如何改判测试名/契约？

不依赖本答复的 residual-watermark、managed fixture、四个产品承重点 cut 与 nm 记账继续推进。
