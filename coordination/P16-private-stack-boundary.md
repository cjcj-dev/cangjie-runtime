待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。

P16 续轮 f03433bb2：私有栈守卫输入可达性请裁。

任务要求逐 guarantee 真实入口控制。ZGC zMark.cpp:1022-1035 的 all-thread/worker 私有栈检查已移植。
真实 GC AcquireControl idle 后分别把实际控制线程、实际空闲 old worker 的 markStacks[1] 安装一个 stale entry，再 RunTo AFTER MARKING STARTED。两种输入均不是目标 Thread marking stack 守卫，而是 Shared marking stripes 守卫：
kkk2:/root/sym_cangjie_runtime_627_implement_r5744767112-cont-weak1/unit-private-default/test-logs/000438-main.log；unit-worker-testable/test-logs/000559-main.log。rc1，VERIFY_TARGET_ASSERT_EXECUTED matched=0，保留 maps/pc_mod/off。

源码原因：concurrentGCBreakpoints.cpp:49 控制线程 ScopedEnterSaferegion 会先发布自己的栈；另外 zGeneration.cpp:393 ZGenerationYoung::mark_start 在两代 Mark().Start(:401,:1010)之前无条件 FlushAllGenerations，worker 私有栈亦发布到共享stripe。故现有真实 breakpoint 不能把私有状态保留到 start 的 Thread 守卫。没有调整目标断言为共享栈，没有提交这个失败实验；原代码保留 coordination/P16-private-stack-experiment.cpp.txt。

问题：是否接受该 start 私有栈守卫的源码支配证明 + 既有共享栈入口断线，明确不能独立触发私有守卫（与 Old holder 必须 old 的已裁例外同类）？worker try_terminate_flush 仍独立待补，不把支配推论外推到它。若必须独立实测，计划用 GDB 在真实 GC 的 Mark.Start / TryTerminateFlush 入点停住后修改预先分配的实际 worker 私有栈状态，再继续真实产品；不加产品 hook，不将测试辅助函数当被测实现。请确认此方式可作为合法输入注入。
