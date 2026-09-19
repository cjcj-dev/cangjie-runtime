待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

本包是 harness/SDK 基础设施，ZGC 无对应机制，不改 runtime 产品码。
基线：5839a9845ae2a667ddb6530646e31f65a139e298。

实施前的接线：
- run_finalizer_trigger.sh:35 / run_segmented_array_managed.sh:44 / run_phase_entry_trigger.sh:64,67：调用 CJC_BIN，由 CJC 环境优先决定；set -e 令编译失败沿 wrapper rc 返回。
- kkk2_managed.sh:79：实际执行 wrapper，:80 读取混合 rc，:125 写入运行数组。
- kkk2_diff.sh:108：把 managed 形状正确视为已运行；:149-156 据 rc 决定差分资格。

修复位置：wrapper 编译器调用前通过既有 CJC 输入安装记录代理；原始 cjc rc 及预期 ELF 存在性必须在写入运行数组前分流。diff 在计算可用差集前检查 build_fail。
承重点切刀：去除 CJC 代理接入（生产），或移除 diff build_fail 判定（消费）。测试只证明装置本身，不外推到 GC 产品行为。
