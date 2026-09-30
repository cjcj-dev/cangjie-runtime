LANE=sym_cangjie_runtime_1315_implement_r5893010973
ROLE=implement
PROGRESS=WIP

问题：本棒绿/切刀/恢复臂并发构建与唯一两构型脚本的固定目录发生工具约束冲突。
证据：/root/cj_build/ops/bin/kkk2_build_two.sh:24 固定 root=/root/__LANE__；:25 删除 default/testable；:28-43 只在同一 root 创建这两个构型。以完整棒名同时调用会互相删除；改棒名又违反产物目录约束。
已完成：两构型绿色产品编译，中间态已提交，测试迁移进行中。未绕过脚本构建。
拟执行的具体方案：两构型继续只用 kkk2_build_two.sh。单构型故障注入臂在 /root/sym_cangjie_runtime_1315_implement_r5893010973/ 下创建 cut-mark/cut-descriptor/cut-promotion/restored 的独立源码/构建目录，以 helper 的同一 CMake 配方（Release/default、同编译器、ccache、prefix-map、GC_UNIT_GATE_SKIP=1）并发构建；只用相同测试 ELF 运行，捕获每臂两枚 SO 与 ELF 哈希、核域和 uptime。全部经 box.sh、ulimit -c 0，无共享 SDK 改动。
请裁决：这种仅对单构型故障注入臂的独立目录/同配方并发构建是否按现行合同授权？若不允许，请指定保持完整棒名且不会相互覆盖的现役入口。等待期间继续完成测试迁移、绿臂与报告，不交 DONE。
