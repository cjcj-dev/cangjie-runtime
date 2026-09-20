待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# #733 OHOS cycleRef 真实对象链的接线边界（WIP）

坐标基于 `530b6953c1067ff4ed084d26a35e43c7d0dbac41`。
ZGC 对应：无对应物（基础设施差异：跨 VM interop），依据本条 issue 特定授权。

## producer → consumer 顺序（产品改动前记录）

| 顺序 | 产品锚 | 输出／消费及目标不变量 |
|---|---|---|
| 1 | runtime/src/Heap/z/zCrossVM.cpp:292 ProcessExportRoots | 从 export owner 遍历引用，ForeignType 对象进入 discoveredExternObjects；fixture 不能替换发现结果声称真实 producer 已通过 |
| 2 | runtime/src/Heap/z/zCrossVM.cpp:240 PrepareCycleRef | splice 进入 cycleRefWorkStack，producer 刀可切 247 行 |
| 3 | runtime/src/Heap/z/zCrossVM.cpp:199 PostResolveCycleTask | 通过 CJ_MRT_RolveCycleRef 投递任务 |
| 4 | runtime/src/CjScheduler.cpp:853 ResolveCycleRefImpl | 经 Heap::ResolveCycleRef 进入产品 resolver |
| 5 | runtime/src/Heap/z/zCrossVM.cpp:88 GetCrossRefHandler | CJForeignProxy→CJInteropContext→CJFunc，两次产品引用屏障读取，最终取得 handler |
| 6 | runtime/src/Heap/z/zCrossVM.cpp:177 ResolveCycleRefStub | 在释放 cycleWorkStackMtx 后进入 managed handler；回调参数必须来自真实链，并进入断言 |
| 7 | runtime/src/Heap/z/zCrossVM.cpp:417 VisitSurrectedExportRoots | 持锁枚举 owner 和 foreign proxy；consumer 刀可切 419 行，目标应对实际输出断言 |

## 需要真实 OHOS 执行臂闭合的 fixture

1. 通过实际 managed ABI 创建 ExportObject（id 对应产品 RegisterExportRoot 的索引）及 ForeignProxy/InteropContext/CJFunc 对象，描述正确的引用布局并用产品引用写屏障连接；不能重新定义 GetCrossRefHandler、替换回调入口或添加产品测试钩子。
2. handler 函数需遵守 OHOS 的 managed ABI，不能从 C++ 函数指针推定 N2CStub 调用约定兼容。由真实生产入口进行发现、准备和投递，执行投递的任务。
3. 验证产品传给 handler 的 export owner/proxy 身份；在 handler 可暂停的点验证 cycleRoot 输出及锁可用性；保留已有有效 cycleRoot 断言。
4. 用同一测试 ELF 比较绿／producer 断线／consumer 断线／恢复；目标断言输出可见，绿 SO=恢复 SO≠切刀 SO。

## 当前资格限制

runtime/config.cmake:444-459 与 ohos_host_stubs.cpp:10-15 明确 host 是 Linux 混合构型。其三项 OHOSCycle 测试可以证明 host 控制流，不证明真实 OHOS producer。
主控裁决允许 host 产品路径原型，真实 OHOS 资格 NOT_RUN 原因=#773。裁决路径：/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_733_implement_r5750309287-20260920T141132Z.md。
fixture 经 RegisterExportRoot → major GC → ProcessExportRoots → PrepareCycleRef → PostResolveCycleTask → 真实投递任务 → GetCrossRefHandler → ResolveCycleRefStub；不手工填中间 map。
