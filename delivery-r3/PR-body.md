## 问题与修复

old mark-start 推进页序号后，pinned 分配仍可能从生命周期列表头向旧 birth 页继续 bump。改为每龄 allocator 持有独立 pinned 快捷页，并在既有 RetireSharedPages 步骤与 shared 页一起退休；保留 mark 后 selection 与资源清理。

对应 ZGC：zObjectAllocator.cpp:196/224、zGeneration.cpp:1222。承接前轮 P1 位图、统一 mark 入口及 birth 修复；本次仅处理 Review R1。

## 验证

- default/testable 并行构建均 rc=0。
- 新真实 GC 窗口测试：绿/恢复通过；撤销退休与恢复列表头消费均在 birth/owner 目标断言失败，独立控制保留。
- testable 729 项：728 通过，仅保留既有 P3 ValueRootCurrentization 失败；default/filler 各 533 项、532 通过。
- OHOS-host 构建成功；runner 因归档缺 .git 退出128，未完成运行。
- 基线 cut 校验 rc=0；同 ELF/SO 血缘、故障差集和原始日志见 delivery-r3/REPORT.md。

报告：/root/cj_build/reports/REPORT-sym_cangjie_runtime_606_implement_r5675167676.md
PR 保持 #628，提交独立 Review；不授予含缺验结果的合并许可。
