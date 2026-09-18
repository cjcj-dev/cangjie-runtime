# #700 正常 runtime workers teardown 测试

本次修改：
- runtime/tests/gc_unit/gc_unit_main.cpp：RunAll 返回值保存，若 Heap::heap()!=nullptr 则调用真实 Heap::StopGCWork，最后返回原 rc；不提前构造 Heap，不使用 _exit，保留 RunAll 全部断言/计数/sentinel 输出。
- runtime/tests/gc_unit/test_gc_thread_pool.cpp：新增 GC_OTHER_VM_TEST(RuntimeWorkers, HeapStopJoinsRuntimePool)，从真实 ZCollectedHeap::heap()->safepoint_workers() 取 pool，先断言真实创建/活动数非零，再两次调用 Heap::StopGCWork 后断言两计数为零。exec 子进程隔离停线程影响，验证真实产品结果，不造 pool 副本。

入口枚举：rg -n '^int main|RunAll\(' runtime/tests/gc_unit。clear_entries_product_unit.cpp 没有第二个 main；两个三臂 ELF 都链接 gc_unit_main.cpp（run_standalone.sh:293、390；OHOS 亦在 :82 使用同入口）。因此共享 main 一处修复覆盖两枚三臂 ELF，无需向 publication 源码添加 main。

其他显式 main：slot_domain_driver.cpp:161 经 InitCJRuntime/FiniCJRuntime 正常入口管理 runtime；timer_ledger_contract.cpp:22 只测 Timer/GcLog，不构造 Heap；red_proof.cpp:80 为独立模型对照，未构造 Heap。本次不扩改它们。

原 379/55 的退出异常仍保持原失败记录；新增正常 teardown 补缺不等于已证实其原因或已修复。未运行本新增测试，待父线程统一构建/断线/恢复验收。
