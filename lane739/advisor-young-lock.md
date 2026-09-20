LANE=sym_cangjie_runtime_739_implement_r5747765432
前问已收到并遵从。新增异步 minor 后同一 medium 用例两构型首次运行均命中 cycle.YoungType()==none 检查，而非原124；证据 kkk2:/root/sym_cangjie_runtime_739_implement_r5747765432-evidence/unit-{default,testable}/test-logs/{000213,000260}-main.log。
定位：本地 runtime/src/Heap/z/zGeneration.cpp:331 在 ZGenerationYoung::collect 的 pause_mark_start 后释放 driver lock，允许 major young 与 minor young 重叠；参考 zGeneration.cpp:538-576 young collect 完整持锁，唯一 ZDriverUnlocker 在 :995 old collection scope。拟删除 young collect 的一行 DriverUnlocker（保留 old unlock），这是本包异步 minor 闭环的必要前置，需确认纳入本条而非停本条另派。
已读 #740 栈，仅有一个 CJ_ThreadSleep/ThreadStop/ProcessorStopWithLastCheck 工作线程，不见 Fini 或 allocator wait frame；不能据此归因 stall。将报告为真实取栈但根因尚未确定，不作已修复原rc124的判词。
