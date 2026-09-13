# A15 phase request control

坐标基于 `403916767341fd0610fdc7a1201f1a0333e0da38`；派发冻结基线 `78fc9ce028de705b3ea705b7069759c1036a2796`。本文件为本棒交付说明，协调面裁定见 evidence/a15/advisor-*.md。

## 函数对应表

ZGC 根为 `/root/cj_build/reference/jdk/src/hotspot/share/gc/`，我方根为 `runtime/src/Heap/`。

| ZGC file:line | 我方 file:line | 不变量 |
|---|---|---|
| shared/concurrentGCBreakpoints.cpp:48,58 | z/concurrentGCBreakpoints.cpp:17,23 | 请求由 runTo/wantIdle/stopped 三态表示；reset 清除三者 |
| shared/concurrentGCBreakpoints.cpp:66,84,97 | z/concurrentGCBreakpoints.cpp:27,37,38 | 获取控制、恢复至 idle 都等待当前周期结束 |
| shared/concurrentGCBreakpoints.cpp:88 | z/concurrentGCBreakpoints.cpp:39 | release 清请求并唤醒暂停的 GC |
| shared/concurrentGCBreakpoints.cpp:101 | z/concurrentGCBreakpoints.cpp:46 | idle 时请求 major，等待命中或周期结束 miss |
| shared/concurrentGCBreakpoints.cpp:134 | z/concurrentGCBreakpoints.cpp:66 | 名字不匹配直接通过；命中唤醒请求者并暂停 |
| shared/concurrentGCBreakpoints.cpp:157 | z/concurrentGCBreakpoints.cpp:76 | pending runTo 在周期末变为 wantIdle，RunTo 返回 false |
| shared/concurrentGCBreakpoints.cpp:172; z/zBreakpoint.cpp:43 | z/zBreakpoint.cpp:18 | 同一锁下等待 startGC 并置 active |
| z/zBreakpoint.cpp:35 | z/zBreakpoint.cpp:10 | 受控状态下发出一次 startGC |
| z/zBreakpoint.cpp:53,57,61,65 | z/zBreakpoint.cpp:27,28,32,36 | 周期结束及三个原名通知 |
| z/zDriver.cpp:360 | z/zDriver.cpp:321 | wb_breakpoint 独立原因，startGC 后异步提交 major port |
| z/zDriver.cpp:471,486 | z/zDriver.cpp:272,282 | 仅 major driver 通知周期边界，abort 不伪造正常完成 |
| z/zGeneration.cpp:1088 | z/zMark.cpp:519 | 旧代 mark_roots 之前通知 AFTER MARKING STARTED |
| z/zGeneration.cpp:1091 | z/zGeneration.cpp:1111 | 首次 mark follow 后、mark-end 重试前通知 BEFORE MARKING COMPLETED |
| z/zGeneration.cpp:1127 | z/zGeneration.cpp:646 | 旧代 non-strong references 开始处通知 |
| z/zGeneration.cpp:1018-1035 | z/zGeneration.cpp:1100 | DoTracing 编排从 zMark 移入 generation，除通知外保持函数体 |

共享 monitor 用 std::mutex/condition_variable；控制方等待前进入本运行时 saferegion，对应 HotSpot MonitorLocker 的可 safepoint 等待。ZGC CodeCache deferred unloading 清理没有我方对应组件，未创造适配器。控制方请求必须串行，名字有效期涵盖 RunTo 调用；release 不得与请求并发，与参考前提一致。

## 产品接线证明

| 控制 API / 测试 | 实际调用链 |
|---|---|
| AcquireControl / RunToIdle | RunToIdleImpl → 等待 major 的 AtAfterGC → NotifyActiveToIdle |
| RunTo / SimpleCycle | RequestGC(WB_BREAKPOINT) → StartGC + major port → ProcessDriverRequest → ExecuteDriverRequest → combined young roots / old collection → TraceHeap → DoTracing → ProcessOldNonStrongReferences |
| ReleaseControl | ResetRequestState → condition.notify_all → At 的等待返回 |
| EndBeforeBreakpoint / UnknownBreakpoint | RunTo 的请求未命中 → major 正常完成 → AtAfterGC → pending 转 wantIdle → false |

停止在命名点时控制权保留；下一次 RunTo、RunToIdle 或 ReleaseControl 恢复执行。shutdown/abort 不额外发明 cancel 状态，也不通知正常周期完成，遵循 advisor-order.md。进程终止前由控制者完成或释放控制；不承诺并发 shutdown 取消请求。

## 删除清单与测试增删

删除原 `z/zRelocate.cpp` 的 RunRemapWindowTestHook 调用，旧层 `Collector/zRelocate.cpp:279-296` 的 setter/状态/dispatcher，以及 `z/zPageAllocator.cpp:53` 的遗留声明。逐处基线行号、原文、候选 git grep rc=1 及基线阳性 rc=0 见 `evidence/a15/source-evidence.json` 的 deleted。没有新增 MRT_GCV2 开关。

按 advisor-replacement.md 删除 `runtime/tests/gc_unit/remap_window_fixture.hpp` 及 include。原 forwarding 场景覆盖面随删除消失、无替代；不是被三个 phase 测试覆盖。精确测试名集合差见同一 JSON 的 tests.added/deleted。

同批移植 `test/hotspot/jtreg/gc/TestConcurrentGCBreakpoints.java`：testSimpleCycle → SimpleCycle（两周期并加入 ZGC reference-processing 点）；testEndBeforeBreakpoint → EndBeforeBreakpoint；testUnknownBreakpoint → UnknownBreakpoint。Java 异常包装 testEndBeforeBreakpointError 无独立 C++ 包装；底层 false 结果由 EndBeforeBreakpoint 断言。新测试链接产品请求 API，不复制状态机。按 alignment_mode 不运行测试，不声称行为验收。

独立 RelocationRequestQueue::SetWaitEnterHook 和 mark closure observer 不属于被 phase 控制替代的这组散点；本包未改它们，未声称它们与 ZGC 等价。

## 构建与限制

唯一入口 `/root/cj_build/ops/bin/kkk2_build_two.sh`；default/testable 独立目录并行，`-j$(nproc)`，由 box 注入 192 核设置。摘要见 `evidence/a15/build.log`，产物与两端 uptime 留在 kkk2 同名 lane 目录。不跑 unit/gate/切刀；构建结果不作为回收功能验收。

## FALSIFIED

先前打算在 DoTracing 开头发 AFTER MARKING STARTED，顺序实读发现它已晚于根枚举；经 advisor-order.md 改到 TraceHeap 入口。旧 relocation fixture 不能由三个标记相位等价替代，按裁定删除并记录覆盖损失。

构建实测：default configure/build=0/0，wall=62s；testable=0/0，wall=61s。两臂并发，nproc=192。runtime SO 分别为 `60f6a040b66eeb4c9a8a16dc184ce25122df1ff723bca4098bf813521edc5031`、`62a20232621e61051719cd72d4d4c8c80e869a3b6d8fc90fa730e9a1559e2c1f`；boundscheck 两臂为 `f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883`。测试未构建运行，没有测试 ELF 或行为通过结论。

交付前再次 fetch/merge 返回 Already up to date（rc=0）。主线 A10b 特征符号按同一 git grep -c 尺两侧累计：RunYoungCollection 5/5、SelectTenuringThreshold 3/3、GenerationCycle:: 13/13、DriverUnlocker 6/6、ShouldPrecleanYoung 3/3、GetYoungDriverPort 14/14；逐文件原文与 rc 在 source-evidence.json。
