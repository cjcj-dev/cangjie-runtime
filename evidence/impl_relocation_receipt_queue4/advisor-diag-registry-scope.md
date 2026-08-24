# impl_relocation_receipt_queue4: TESTABLE macro 登记范围裁决

ROLE=implement。`TESTABILITY_MACRO_GATE.md` 建议将新编译期宏 `MRT_TESTABLE_INTERNALS` 加入
`/root/cj_build/tools/diag_registry.sh` 名册；但本轮硬边界又要求“范围同原任务书”，而该共享工具
不在 cangjie_runtime 工作树/本分支内。

请裁决：

1. 本棒是否允许越出产品树修改共享 `tools/diag_registry.sh`；或
2. 本棒只在 runtime CMake/config 中登记真正编译期 option，由主控/合并棒另行同步共享名册。

在裁决前，我不修改共享工具，继续完成 runtime 范围内不依赖此答复的工作。
