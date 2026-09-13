待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# A10b：双 driver 与 generation owner

本轮角色 implement；形态对齐，不含 gate/unit/切刀执行或并发运行验收。
冻结基线 `3c3216a7293b28690411a6283c7fd74076bdcd1b`；已按内容合入主线 `78fc9ce028de705b3ea705b7069759c1036a2796`。
R 根：`/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_494_implement_r5655389950/`；Z 根：`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`。
表中是本包修改的机制或调用参数对应，不把未改的外层标记、搬移、互操作及诊断函数声明成逐指令移植。

## 函数对应表

| R file:line | Z file:line | 本包范围 |
|---|---|---|
| runtime/src/Heap/z/zDriver.cpp:141 | zDriver.cpp:201 | young 循环：receive → driver lock → collect → ack；major 同一循环按独立 port 实例运行，对应 ZDriverMajor::run_thread:463 |
| runtime/src/Heap/z/zDriver.cpp:267 | zDriver.cpp:463 | 锁覆盖 collect、abort 判断及 ack；old collect 内部短暂释放 |
| runtime/src/Heap/z/zDriverPort.cpp:176 | zDriverPort.cpp:137 | 阻塞等待请求；关闭端口唤醒，沿用既有 cooperative abort |
| runtime/src/Heap/z/zDriver.cpp:195 | zDriver.cpp:442 | 按 cause 选择 full preclean→full roots 或 partial roots，再进入 old；各代 quota 明确指定 |
| runtime/src/Heap/z/zDriver.cpp:170 | zGeneration.cpp:379 | 计时和 worker 累计从指定 GenerationCycle 取得 |
| runtime/src/Heap/z/zDriver.cpp:434 | zGeneration.cpp:514 | 周期前后处理；旧 CopyCollector 层定义移入 zDriver；不再持整周期 STW lock |
| runtime/src/Heap/z/zDriver.hpp:152 | zDriver.cpp:91 | 共享 driver 锁 RAII |
| runtime/src/Heap/z/zDriver.hpp:162 | zDriver.cpp:99 | old 主体释放；退出时重取 |
| runtime/src/Heap/z/zGeneration.cpp:961 | zGeneration.cpp:1015 | old mark/mark-end/non-strong/reset/select/relocate 主体在解锁作用域 |
| runtime/src/Heap/z/zRelocate.cpp:374 | zGeneration.cpp:1055 | young-root remap 与 old relocate-start 同一重取锁区间；remap 在 pause 前 |
| runtime/src/Heap/z/zGeneration.cpp:93 | zGeneration.cpp:538 | young 周期继续持 driver lock；phase 与 worker/stats 显式 YOUNG |
| runtime/src/Heap/z/zGeneration.cpp:116 | zGeneration.cpp:583 | combined roots 前奏在同一 young pause 建立 old 起点，已有代码保留 |
| runtime/src/Heap/z/zGeneration.hpp:32 | zGeneration.cpp:124 | 每代持 phase/sequence/reason/active、GCWorkers、GCStats、ZStatCycle |
| runtime/src/Heap/z/zGeneration.cpp:50 | zGeneration.cpp:855 | young seq 在 mark-start 与 remset flip 一起推进 |
| runtime/src/Heap/z/zGeneration.cpp:918 | zGeneration.cpp:1212 | old seq 在 combined mark-start 的 Begin 推进；young 不在 Begin 推进 |
| runtime/src/Heap/z/zGeneration.cpp:75 | zGeneration.cpp:1379 | 明确代 phase 发布，old relocate-start 捕获 young seq |
| runtime/src/Heap/z/zGeneration.cpp:932 | zGeneration.cpp:373 | 指定代 phase 原子发布；沿用我方 pause 子阶段名称 |
| runtime/src/Heap/z/zGeneration.cpp:903 | zGeneration.inline.hpp:46 | 一次读取同一 owner 的周期元数据；不是单 active owner 路由 |
| runtime/src/Heap/z/zGeneration.cpp:910 | zDriver.cpp:110 | 原因由指定 generation 保存 |
| runtime/src/Heap/z/zGeneration.cpp:938 | zGeneration.cpp:386 | 只结束本代 activity |
| runtime/src/Heap/z/zGeneration.cpp:947 | zGeneration.cpp:145 | worker 存储由 generation 所有 |
| runtime/src/Heap/z/zGeneration.cpp:954 | zCollectedHeap.cpp:106 | 停止协议的资源端：driver join 后释放 generation 所有 worker；ZGC 对应 stop/abort 调用层 |
| runtime/src/Heap/z/zCollectedHeap.hpp:63 | zGeneration.inline.hpp:66 | mutable/const owner 查询与 phase/snapshot/stats 入口 |
| runtime/src/Heap/Collector/CollectorProxy.h:41 | zGeneration.inline.hpp:70 | 所有代理读写都到真实 collector 的 generation，包括 remset seq 比较 |
| runtime/src/Heap/Collector/CollectorProxy.cpp:25 | zGeneration.inline.hpp:66 | 请求不再重复写共享 currentCollector；只调用初始化时绑定的对象 |
| runtime/src/Heap/z/zDriver.cpp:414 | zGeneration.cpp:145 | 按显式 generation 取得 owner 的 worker |
| runtime/src/Heap/z/zDriver.cpp:419 | zGeneration.cpp:124 | 旧统计访问兼容静态 old 默认；young 调用显式选择 YOUNG，不存在 active 路由 |
| runtime/src/Heap/z/zDriver.cpp:426 | zDriver.cpp:129 | 只读两代 activity 聚合用于堆活动查询，不参与选择代 |
| runtime/src/Heap/z/zDriver.cpp:103 | zDriver.cpp:490 | 分别关闭两个端口，再 join 两个 driver，沿用 abort 清理 |
| runtime/src/Heap/z/zDriver.cpp:325 | zDriver.cpp:118 | 各自线程与 worker owner 初始化 |
| runtime/src/Heap/z/zDirector.cpp:97 | zGeneration.cpp:153 | 采样/resize 都到明确代的 CycleStats/Workers；director 规则本体保留 |
| runtime/src/Heap/z/zGeneration.cpp:1010 | zGeneration.cpp:379 | phase/reason/workers/stats 接口传代 |
| runtime/src/Heap/z/zGeneration.cpp:1043 | zGeneration.cpp:386 | 只结束该代 worker；phase 握手显式选择代 |
| runtime/src/Heap/z/zForwarding.hpp:365 | zGeneration.inline.hpp:38 | 读 young owner；ENUM/CLEAR/TRACE 为已有 Mark 子阶段 |
| runtime/src/Heap/z/zHeap.cpp:232 | zGeneration.inline.hpp:38 | Heap/Collector GetGCPhase 与 SetGCPhase 均强制 generation 参数 |
| runtime/src/Mutator/MutatorManager.cpp:1226 | zGeneration.cpp:608 | 将 generation 传入现有 mutator 握手；STW lock 只包相位操作 |
| runtime/src/Mutator/MutatorManager.cpp:1073 | zGeneration.cpp:1198 | old pause 明确 OLD，并设置该次 mutator 操作的 EnumYoung=false |
| runtime/src/Mutator/MutatorManager.cpp:361 | zBarrierSet.cpp:253 | 新加入线程继承现有 root operation 的 generation；不从某个 active cycle 推断 |
| runtime/src/Mutator/Mutator.cpp:298 | zGeneration.cpp:608 | ack 从本次操作的 EnumYoung 查询正确代 phase |
| runtime/src/Mutator/Mutator.inline.h:44 | zGeneration.cpp:608 | phase 转换消费者同样读取操作指定的 generation |
| runtime/src/Heap/Collector/Collector.cpp:303 | zPage.inline.hpp:119 | 对象相关消费者按页 owner；删除 active forwarding generation |
| runtime/src/Heap/z/zStoreBarrierBuffer.cpp:22 | zStoreBarrierBuffer.cpp:143 | pending holder 读取页 owner 的 relocate phase；buffer 算法保留 |
| runtime/src/Heap/z/zThreadLocalAllocBuffer.cpp:185 | zGeneration.inline.hpp:38 | 删除 heap/global mutator TRACE 到 young marking 的桥；读取分配页 owner |
| runtime/src/Heap/z/zRelocate.cpp:362 | zGeneration.cpp:1379 | young/old 调用点显式传 generation，不再从原因猜测 |
| runtime/src/Heap/Collector/CopyCollector.cpp:57 | zGeneration.cpp:1202 | 共享执行器携带 generation；明确选择对应 worker |
| runtime/src/Heap/z/zRelocate.cpp:2183 | zGeneration.cpp:1202 | 清理筛选由传入 generation 决定，不读取共享统计 reason |
| runtime/src/Heap/z/zDriver.cpp:513 | zGeneration.cpp:514 | young type RAII 包围整个年轻代周期 |
| runtime/src/Heap/z/zDriver.cpp:519 | zDriver.cpp:270 | 显式 full 原因或旧代分配等待时 preclean；其余原因 partial roots |
| runtime/src/Heap/z/zGeneration.cpp:1073 | zGeneration.cpp:489 | 类型 none→指定类型→none，作用域结束恢复 |
| runtime/src/Heap/z/zGeneration.cpp:1088 | zGeneration.cpp:704 | full preclean 阈值 0；其余类型沿用计算算法 |
| runtime/src/Heap/z/zDriver.cpp:542 | zGeneration.cpp:608 | 根据 young type 选择 minor 或 major 的操作上下文 |
| runtime/src/Heap/z/zMark.cpp:795 | zGeneration.cpp:886 | 只有 major roots 发布 old 根；preclean 不提前开始 old mark |
| runtime/src/Heap/z/zMark.cpp:516 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |
| runtime/src/Heap/z/zMark.cpp:1151 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |
| runtime/src/Heap/z/zMark.cpp:1302 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |
| runtime/src/Heap/z/zMark.cpp:1633 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |
| runtime/src/Heap/z/zMark.cpp:1669 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |
| runtime/src/Heap/z/zRootsIterator.cpp:303 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |
| runtime/src/Heap/z/zRelocate.cpp:1069 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |
| runtime/src/Heap/z/zRelocate.cpp:1592 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |
| runtime/src/Heap/z/zRelocate.cpp:1867 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |
| runtime/src/Heap/z/zRelocate.cpp:1884 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |
| runtime/src/Heap/z/zRelocate.cpp:2352 | zGeneration.inline.hpp:66 | 本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明 |

Heap dump 例外由 advisor 明确批准：`Heap::DumpHeap`（R `runtime/src/Heap/z/zHeap.cpp:449`）与三个请求线程调用点对应 `/root/cj_build/reference/jdk/src/hotspot/share/services/heapDumper.cpp:2640` 的 `VM_HeapDumper::doit`、`:2883` 的 VMThread 调用层。我方没有 VMThread，沿用 `CjHeapData::DumpHeap`（R `runtime/src/Inspector/CjHeapData.cpp:93`）及 IDE `Serialize`（R `runtime/src/Inspector/HeapSnapshotJsonSerializer.cpp:22`）内部已有 STW；driver 不再消费 Inspector 队列。

线程入口（CjScheduler、UnwindCApi）、AArch64 TLS、现有诊断的 phase 读取只做必需的显式 owner 参数传播，对应 Z `zBarrierSet.cpp:253` 的线程附着／Z `zGeneration.inline.hpp:66` 的 owner 查询；不扩展这些包外函数机制。完整调用原文见 `evidence/a10b/owner-consumers.txt`。

## 双边相位与共享状态表

| ZGC 阶段 | 我方调用链 | driver 锁 / 共享状态 |
|---|---|---|
| minor run_thread → young collect (zDriver.cpp:201; zGeneration.cpp:538) | RunDriverLoop(MINOR) → ProcessDriverRequest → DoYoungGarbageCollection | 整个 young 持 driver 锁；young phase/seq/workers/stats 独立 |
| full preclean (zDriver.cpp:416-423; zGeneration.cpp:704/819) | RunYoungCollection(major_full_preclean) → SelectTenuringThreshold=0 → 既有晋升消费端 | 仍持 driver 锁；不建立 old mark，不记录普通 young 周期统计 |
| major collect_young roots (zDriver.cpp:416; zGeneration.cpp:583) | ExecuteDriverRequest 选择 full/partial roots → young pause 内 oldCycle.Begin + young.StartYoungMark | 同一 pause 建立两代起点；standalone minor 等 driver 锁 |
| old concurrent mark (zGeneration.cpp:1015-1020) | DoGarbageCollection(OLD) 的 DriverUnlocker → TraceHeap | 可与 young Mark/MarkComplete/Relocate 重叠；各自 phase/worker/stat，暂停由现有 syncMutex 串行 |
| old mark-end / continue (zGeneration.cpp:1021-1029) | TraceHeap → DoTracing → TryEndOldMark / TracingImpl | driver 锁仍释放；mark-end 使用既有 STW；重试操作指定 OLD |
| old mark-free / non-strong (zGeneration.cpp:1030-1039) | DoTracing → ProcessOldNonStrongReferences | old worker/domain；共享 resurrectionBlocked 对应 ZResurrection，沿用既有机制 |
| old reset/select (zGeneration.cpp:1040-1052) | PostTrace | 可与 young 周期重叠；allocator/forwarding 承重点本体保留，只迁移状态 owner |
| old remap young roots → pause relocate start (zGeneration.cpp:1054-1063) | Preforward 的 DriverLocker → RemapYoungRoots → ScopedLightSync → flip_old_relocate_start | 这两步同锁；不与 young 周期重叠；捕获 young seq 通过真实 proxy owner 读取 |
| old concurrent relocate (zGeneration.cpp:1066-1071) | ForwardFromSpace(OLD) → FinishIncompleteFromRegions(OLD) | driver 锁释放；可与 young 各阶段重叠；worker、phase、清理代显式指定 |
| old at_collection_end (zGeneration.cpp:1006-1009) | DoGarbageCollection 作用域返回重取锁 → PostGarbageCollection(OLD) → cycle.End | 只结束 old；ack 在 driver 锁内 |
| stop / resize | StopGCWork → 两 port.Stop → join 两 driver → 两 generation.StopWorkers；EvaluateDirector → 指定代 RequestResize | 不再向废弃任务队列发送退出任务；runtime safepoint worker 在 driver join 后释放 |

`GCPhase` 的 ENUM/CLEAR/TRACE 是既有 mutator pause/handshake 子阶段，均映射 ZGC Mark；本包没有加入新的阶段、路径选择开关或配置。`epochHandshakeGeneration` 只携带当前串行 root operation 的参数供新线程加入，受原 ledger mutex 保护，不选择或合并 generation 周期状态。

## 删除清单与可复跑证据

`evidence/a10b/deletions.txt` 保存每项冻结基线阳性对照及候选 `git grep -n -F` 原文、各自 rc；`deletions.json` 为同一结果的结构化形式。
删除：ActiveCycle/activeCycle/SelectCycle/GetCycleReason、ActiveForwardingGeneration、driverRequestActive、单线程 RunTaskLoop/GCMainThreadEntry、driver TaskQueue 字段、PostIgnoredGcRequest/HasSyncTaskCompleted/finishedGcIndex/isGcStarted、currentTagID/FlipTagID/GetCurrentTagID、driver 持有的 youngWorkers/oldWorkers、无参数 GetGCPhase。MRT_GCV2_MINOR_DEFERS_HEU 与 D07 的删除一致保留。
替代分别是 generation owner、独立 port receive/ack、generation-owned worker/stat、显式 phase 参数和已有每代颜色翻转。另删除 youngPreludeRequest/YoungPreludeRequest 单次前奏指针，以 ZYoungType 和 old owner 的 requestIndex/reason 取代。不存在新 MRT_GCV2_* 开关。

## 主线内容核

`evidence/a10b/main-content.json` 保存真实 `git grep -c` 文本和 rc：HandshakeTimeout、FollowArrayElements、FollowPartialReferences、RecordMajorGCFinish、FORWARDING_FACE_RESET_BIT、MarkFaceMatchesOwner、TryRecoverInteriorBase 七组特征均保留。四处冲突只涉及旧定义迁移和 D07 删除；删除同样应用于迁移后的函数。

## 测试同批迁移

无专门 zDriver/zGeneration gtest 的对应测试名可移植；新增 `GenerationState.IndependentPhaseSequenceAndWorkers`、`FullPrecleanPromotesAllAndRootsComputeThreshold` 及 `GcRequestSync.MajorFullPrecleanThenCombinedRoots` / `MajorPartialRootsWithoutPreclean`，分别是 `zGeneration.cpp:125-170` 所有权不变量的源码移植用例，不声称已执行。既有请求测试改用两条产品 driver 循环和端口 ack，既有 phase/worker fixture 改为明确 generation。
`GcRequestSync.MinorAndMajorDriversSerializeCollections` 改名为 `YoungPreludeAndMinorShareDriverLock`，描述它实际覆盖的 minor/major-young 锁面。三个已删除 ignore-route 用例由 `LegacyIgnorePolicyCannotSuppressSyncRequest` 和既有 `CompilerAsyncEntryReturnsAndMergesPendingRequest` 覆盖新端口请求契约；不保留与删除机制相矛盾的期待。`DriverWaitInjectsTimeoutBackupRequest` 改为 `DriverWaitReceivesDirectorRequest`，输入来自端口，不再期待 driver 自己制造定时请求。full 请求的完成/abort/receipt-wrap 用例增加对第二个 young 周期的等待；对应 ZGC preclean→roots→old 的真实顺序。没有改 known_failures、豁免或常量假删除。
两份 GC_TEST/GC_OTHER_VM_TEST 注册名集合差（相对冻结基线及当前主线）见 `evidence/a10b/test-set-diff.json`。不运行测试门，测试执行状态为 NOT_RUN(alignment_mode)。

## 构建与限制

只使用规定 `kkk2_build_two.sh`，default/testable 两臂同时构建，`-j$(nproc)`=192。原样 rc、wall 和 SO 摘要见 `evidence/a10b/final-validated-build.log`；远端完整日志在 `kkk2:/root/sym_cangjie_runtime_494_implement_r5655389950/`。
较早 `driver-build-2.log` 是合并尚未解决冲突时的无效构建尝试，不用于候选结论；最终构建发生在解决冲突之后。中间编译错误保留于 ownership-build*.log；没有故意破坏或运行 gate/unit。
不声明并发运行、性能、Windows/AArch64 构建验收；导出登记及 AArch64 调用点随接口同批更新。

## FALSIFIED

冻结 `3c3216a7293b28690411a6283c7fd74076bdcd1b:runtime/src/Heap/z/zPage.cpp:300` 的 `GetSnapshotEpoch` 已经按页所属 generation 读取 sequence，旧包书的“页 snapshotEpoch 代替 generation seq”不是当前缺口。本包没有重复创建计数。
冻结源码的 `ZForwarding::young_marking` 已经查询 young snapshot，本包删除的 global TRACE 桥在分配路径，不能把旧描述直接归到 forwarding getter。


## major 原因与 producer → consumer

| 我方原因 / 条件 | ZGC cause 对应 | 执行类型 |
|---|---|---|
| USER / FORCE / OOM | java_lang_system_gc / wb_full_gc、dcmd_gc_run / z_allocation_stall | full_preclean → full_roots → old |
| BACKUP、HEU/HEU_SYNC、NATIVE/NATIVE_SYNC，且现有 FIFO 有分配等待 | timer / allocation_rate / metadata threshold，is_alloc_stalling_for_old | full_preclean → full_roots → old |
| 上述自动原因，FIFO 没有分配等待 | should_preclean_young 返回 false | partial_roots → old |
| YOUNG | minor driver | minor |

FIFO 的请求方是 `RegionManager::StallAllocation`，只发 `GC_REASON_OOM`，故这里的等待是等待 major；不是新增等待装置。`ShouldPrecleanYoung` 在 driver 锁内取样，与 ZGC 的 cause/等待判定位置相同。

| 生产点 | 消费点 | 顺序要求 |
|---|---|---|
| ExecuteDriverRequest 为 old owner SelectReason(reason,index) | DoYoungGarbageCollection 的 IsMajorRoots → oldCycle.Begin(requestIndex) | preclean 前准备请求，只有 roots pause 推进 old seq/phase |
| RunYoungCollection 的 YoungTypeSetter | GetYoungDriverPort、NoteYoungMarkStart、mark.cpp 的 IsMajorRoots | type 先于所有 pause、标记根与 abort 读取，周期结束恢复 none |
| GenerationCycle::SelectTenuringThreshold | EvacuateYoungRegions / RelocateObjectInner / CompactRegion / StayYoungThisCycle | preclean 阈值为 0，既有年龄决策消费明确 young owner 的值 |
| PublishGenerationPhase(OLD, PREFORWARD) 捕获 young Sequence | CollectorProxy → GetGenerationCycle(OLD).ActiveRemsetIsCurrent | forwarding/remset 比较来自真实两代 owner，不是代理基类的闲置对象 |

## old × young 共享状态清单

| 状态 | 定义 / 访问点（R） | overlap 处理 / 本包边界 |
|---|---|---|
| phase/seq/reason/requestIndex/activity/young type | zGeneration.hpp GenerationCycle；zGeneration.cpp Snapshot/Begin/PublishPhase/End | young/old 各自保存；type 只描述 young 的当前 collection |
| GCWorkers、GCStats、ZStatCycle | zGeneration.hpp；zDriver.cpp RunCollection；zDirector.cpp EvaluateDirector | 分代持有；full_preclean/full_roots 不污染普通 young cycle 统计 |
| mark domains | zMark.hpp majorMarkDomain；WCollector.h youngMarkDomain | 已有两域保留，worker 参数全部显式；只有 major roots 能从 young 根发布 old 标记工作 |
| minorCandidateRegions 与旧代 forwarding set | WCollector.h；zRelocationSet.cpp；zRelocate.cpp | 原有分代载体保留；本包传 generation/type，不改算法 |
| 颜色位与线程操作上下文 | zAddress.inline.hpp；MutatorManager.cpp TransitionAllMutatorsToGCPhase / RunEpochHandshake | 颜色翻转仍在既有 STW；mutator 的操作参数与新加入线程均携带明确 generation |
| resurrectionBlocked | zDriver.hpp，zGeneration.cpp ProcessOldNonStrongReferences | 保留 ZResurrection 对应共享状态；old 发布，两个代的屏障可读取 |
| allocator region lists / fullTraceRegions / largeTraceRegions | zPageAllocator.hpp；zPageAllocator.inline.hpp:190/206；Heap/Allocator/RegionList.h:19/29/35 | 原分配器组件保留，RegionCache 操作持 listMutex；本轮不声明该组件整体已通过并发运行验证，按 advisor 的“其余机制不动”边界处理 |
| safepoint/runtime workers、syncMutex、root ledger | zDriver.hpp；MutatorManager.h/cpp | 相位操作共用既有同步；整周期 ScopedSTWLock 已删除；driver join 后释放 worker |
| driver 请求 busy/port/abort | zDriver.cpp；zDriverPort.cpp | 两端口独立；minor/major busy 在 directorMutex 下分别更新；按端口 ack/cancel |

本轮没有负载运行结论。共享组件列在这里供随后统一门与对抗审查定位，不把构建成功升级为它们的行为验收。

## 最终构建身份

`evidence/a10b/final-validated-build.log`：构建提交 `60df4f9512102c902c2b11ce84da1cc416127849`，default/testable 的 configure_rc/build_rc 都为 0，各 wall=48s，实际 -j192、并行臂数 2。
完整源码包与 SO SHA256、CMake 提交 stamp、核域以及前后 uptime 见 `evidence/a10b/artifact-metadata.json`。构建前 load average 0.12/0.76/1.37，构建后 1.19/0.95/1.41；这里只记录构建条件。
文档/证据提交不改变 runtime 树，交付时独立比较树哈希。参考测试清单见 `evidence/a10b/zgc-test-inventory.txt`，其中 test_zForwarding.cpp 为文件检索阳性对照。
