堆遍历器原先只检查本地队列是否排空，并直接修改其他 worker 的 vector。现在使用 owner/steal/overflow 队列、共享根迭代状态和 TaskTerminator，所有 worker 达成终止协议后才返回；去重改为 ZGranuleMap + CHeapBitMap，按 acquire 查询、锁内复查、release 发布安装位图。

对应 ZGC zHeapIterator.cpp:254–347、480–530，gc/shared/taskqueue.hpp:153/333/437 和 taskTerminator.cpp:131。新增五项产品入口测试，覆盖多 worker、重复根、跨 granule 汇合、数组续块和溢出队列。

验证：
- default/testable 构建 rc=0/0；default/filler/testable 分别 1359/1359/1513 项通过，三臂 CAND-ONLY=0。
- 同一 ELF 的五刀分别命中终止、可达集合、唯一访问、数组分块、真实根入口目标断言；候选/恢复全绿且 SO 哈希相同，切刀 SO 不同。
- OHOS-host 独立构建及六项测试通过。首次 configure 缺 libc shim 的失败保留，按装置要求补齐本棒副本后通过。
- managed NOT_RUN：两端 N=3 同为 SDK 的 CJ_MRT_RequestStringDedup 链接签名，按既有例外送审，不计通过。

主控已确认原“本地 drained 必导致集合遗漏”判据错误；终止刀改测有工作时不得提前返回，访问集合断言独立保留。

报告：/root/cj_build/reports/REPORT-sym_cangjie_runtime_1305_implement_r5892797534.md
差分：/root/cj_build/reports/DIFF-e6a20f5db0f9-vs-9733dfc09d29.{json,md}
关联 #1305。
