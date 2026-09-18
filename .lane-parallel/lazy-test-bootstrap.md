# #700 gc_unit 首次 Heap 构造归还测试入口

删除 gc_unit_main.cpp 中 PrepareIsolatedGcUnit、PrepareIsolatedGcUnitProcess、过滤参数触发的全局预建调用、isolatedTest 标志及只为该 peer 存在的强制 MRT_TESTABLE_INTERNALS/include 块。保留 filter/list 参数处理、ConcGCThreads/ZGlobalsPointers 初始化、ThreadLocal cleaner 与 ZCPU 初始化；为这些保留调用显式包含 zGlobals.hpp/zAddress.hpp/zCPU.hpp。

读证：runtime/tests/gc_unit/test_segmented_array_init.cpp:1152 的 RunRuntimeCase 在 fork 后的子进程内执行 InitCJRuntime，而不 exec。旧 main 预建 Heap 在此前启动构造期 driver；继承的对象并不携带已运行线程。测试拥有自己的首次 Heap 构造时点，产品 ZCollectedHeap 构造启动语义不回退。GC_OTHER_VM 的 exec 行为未修改。

主线程提供的实际观测证据（本子任务未重跑）：
- kkk2:/root/sym_cangjie_runtime_700_implement_r5723534433-debug/constructor-wait-gdb.log
- kkk2:/root/sym_cangjie_runtime_700_implement_r5723534433-debug/constructor-child-gdb.log

本子任务仅改 gc_unit_main.cpp，未修改测试断言/测试名，未构建或运行测试。若真实失败揭示某用例缺自身 bootstrap，由该用例补真实入口；不得恢复全局预建。
