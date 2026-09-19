待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

本包是 harness/SDK 基础设施，ZGC 无对应机制，不改 runtime 产品码。
基线：5839a9845ae2a667ddb6530646e31f65a139e298。

实施前的接线：
- run_finalizer_trigger.sh:35 / run_segmented_array_managed.sh:44 / run_phase_entry_trigger.sh:64,67：调用 CJC_BIN，由 CJC 环境优先决定；set -e 令编译失败沿 wrapper rc 返回。
- kkk2_managed.sh:79：实际执行 wrapper，:80 读取混合 rc，:125 写入运行数组。
- kkk2_diff.sh:108：把 managed 形状正确视为已运行；:149-156 据 rc 决定差分资格。

修复位置：wrapper 编译器调用前通过既有 CJC 输入安装记录代理；原始 cjc rc 及预期 ELF 存在性必须在写入运行数组前分流。diff 在计算可用差集前检查 build_fail。
承重点切刀：分别断开编译退出码失败分类、成功但缺 ELF 分类（生产），或移除 diff build_fail 判定（消费）。测试只证明装置本身，不外推到 GC 产品行为。


## 0919 20:4x 范围裁定与删除清单
主控 outbox：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_723_implement_r5741705087-20260919T124226Z.md`。
- 删除 `run_arm h48` 目标调用以及 JSON `arms.h48`；H48 仍仅作为 CJC 编译器宿主。
- 删除 H48 target 的 finalizer / segmented / phase 运行项（原有无效红）；不是降低仍有效的目标断言。
- 真实证据：H48 单次完整头文件预演产生两个 phase ELF，但两者加载均 rc=127，缺 `g_cjHeapRangeEnd@CANGJIE`。仅补头文件不能满足染色目标 ABI。
- 染色目标三个用例及其断言全部保留；编译失败仍 build_fail，差分仍 NOT_RUN。
- 同步主控差分服务的 runner sha 隔离目录；防止并发差分覆盖固定 harness 路径。协调面不由本棒写入。
