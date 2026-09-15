待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。
# P07 函数对应及删除清单
坐标：本轮组合候选32a6281d2（完整SHA由交付头给出）；主线91f3dcc232201d4ad98ec3af6f10165edb950416。仅源码对应，不代替构建、动态证据或独立审查。
ZGC根：/root/cj_build/reference/jdk/src/hotspot/share/gc/；我方根：runtime/src/Heap/z/。

| ZGC函数/数据 | 我方对应 | 接续处置 |
|---|---|---|
| shared/workerThread.hpp:39 WorkerTask | workerThread.hpp:25 | 保留P06 name/gc_id/work(id)契约，构造捕获GCId |
| shared/workerThread.cpp:41 coordinator_distribute_task | workerThread.cpp:31 | task→not_finished→start→end.wait→reset；R3断线目标 |
| shared/workerThread.cpp:63 worker_run_task | workerThread.cpp:52 | start.wait→fetch_add id→TLS→GCIdMark→work→fetch_sub→归零end.signal |
| shared/workerThread.hpp:85 WorkerThreads；cpp:93/133/200 | workerThread.hpp:69；cpp:106/137/173 | 独立派发器，无Heap/Mutator调用；线程入口另做运行时线程注册 |
| shared/workerThread.hpp:131 WorkerThread；cpp:211 | workerThread.hpp:118；cpp:185/216 | TLS id以任务参与次序分配，非固定OS线程下标 |
| shared/workerThread.hpp:154 WithActiveWorkers | workerThread.hpp:150 | 临时改变参与数后恢复 |
| z/zTask.cpp:26/38/41/45 | zTask.cpp:11/26/21/31 | 内嵌WorkerTask适配器→无参work；ZRestartableTask默认空resize |
| z/zWorkers.hpp:38 六字段 | zWorkers.hpp:26 | WorkerThreads、generation_name、resize_lock、requested、active、stats |
| z/zWorkers.cpp:45/67/71/75/81/87 | zWorkers.cpp:28/46/51/56/63/70 | 构造建池；锁下改变active与请求 |
| z/zWorkers.cpp:92/108/126 | zWorkers.cpp:77/95/116 | 普通run统计前后包围；重启任务锁内resize；run_all暂时用最大参与数 |
| z/zWorkers.cpp:143/147；zWorkers.inline.hpp:31 | zWorkers.cpp:135/141；zWorkers.inline.hpp:14 | 暴露resize锁；请求早退；原子读请求不加锁 |
| z/zStat.cpp:1330–1405 | zStat.cpp:99–173 | 累计duration/time与在飞batch；本轮不改P15/P16其它统计规则 |
| z/zDirector.cpp:651 | zStat.cpp:250 SampleWorkerResizeStats | resize锁下读取活动状态/参与数，统计来自ZStatWorkers |
| shared/concurrentGCThread.cpp:37/43/55 | concurrentGCThread.cpp:34/58/71 | create/start、run_service后通知终止、stop_service后等待终止 |
| z/zThread.cpp:27/38 | zThread.cpp:13/26 | run_thread后等should_terminate；stop_service通知并terminate |
| z/zDriver.cpp:118/201/227；319/463/490 | zDriver.cpp:58/65/70 | 两driver统一线程外壳；P14拆Minor/Major类与port归位保留原归属 |
| z/zDirector.cpp:73/916/932 | zDirector.cpp:39/45/50 | ZThread派生并委托director循环 |
| z/zStat.cpp:1022/1070/1091 | zStat.cpp:481/496/488 | ZThread派生、采样循环、terminate唤醒 |
| z/zUncommitter.cpp:41/109/171 | zUncommitter.cpp:99/251/120 | Start/Stop线程外壳；保留主线Uncommitter算法，后合P04仍按接口合成 |
| z/zCollectedHeap.cpp:314–319 | zDriver.cpp:110/128 | director→major→minor→stat停止顺序（R4） |
| z/zMark.cpp:830/887/909–918 | zMark.cpp:669/729/YoungStripedMarkingWork/ConcurrentMarkingWork | 各mark任务尾部刷新代栈；派发器无刷新 |
| z/zArguments.cpp:67–81；zValue.inline.hpp:108 | zDriver.cpp:397 | ConcGCThreads先于建池/SMR的ZPerWorker构造；P06消费保留 |

## 基础设施差异（沿PLAN §5与原审查接受范围）
- I14锁/信号量使用C++同层原语；Semaphore采用P06已合入的mutex/CV版本，保留trywait接口（shared/workerThread.cpp:34–83；runtime/semaphore.hpp:33–58）。
- I17没有HotSpot NamedThread/Thread对象：pthread入口做ThreadLocal线程类型与平台名称注册。派发路径不调用Heap/Mutator方法。
- HotSpot线程池存活至VM退出；我方Heap::Fini及进程内夹具会销毁池，因此保留析构退出任务+join。它不恢复GCWorkers Stop三态机（shared/workerThread.hpp:85）。
- ConcurrentGCThread以每实例mutex/CV替代HotSpot Terminator_lock，退出后pthread_join；Uncommitter可重启时复位终止标志（shared/concurrentGCThread.cpp:33–69）。
- I15预算由StartGCThreads算出，因此GenerationCycle保留unique_ptr延后建池、ZWorkers构造收capacity。P14预算前移/按值成员仍属原接受边界（z/zGeneration.cpp:129、zArguments.cpp:67）。

## 删除清单及消费者
| 删除对象 | 消费者处置 |
|---|---|
| zRuntimeWorkers.{hpp,cpp}与配置/诊断/初始化/析构入口（D6=A） | CMake不再编译；资源持有删除；测试调用真实generation ZWorkers |
| GCWorkers Snapshot/GetSnapshot/remainingWorkers/batch等 | driver读ZStatWorkers；director锁下读active；VerifyEmpty(remainingWorkers)移除；测试读产品任务结果 |
| GCWorkers Stop/closing/shutdown/stopped/CheckOpen | 统一ZThread::stop与对象析构；四条旧Stop状态机测试退休 |
| GCWorkers内建elapsedNanos/workerNanos计时 | ZStatWorkers在ZWorkers::run前后接入并支持在飞读取 |
| 池内FlushCurrentThreadMarkStacks | 回到四类mark任务work尾部（P09续接） |
| TsanPosCtrlMaybeRace、SetThreadPriority空函数、反向RuntimeWorkers include | 删除；worker入口保留实际TSAN attach |
| GCWorkers::Generation重复枚举 | 统一GCCycleGeneration；不恢复旧映射 |
| RunCollection的MRT_GC_UNIT_TESTS workers==nullptr绕过 | 删除；测试建立真实generation worker集合 |
| testRootsResult/ObservePublishedRoots及产品RootPublicationSnapshot | 测试侧root_publication_snapshot.hpp读真实发布条带；R1/R2保留MajorSeed原判据、六家族、OHOS根发布观察 |
| zMark.cpp旧workers==0→1/SetActiveWorkers(1)兜底 | 删除；池构造保证合法非零参与数。补R5遗漏 |
| 旧pthread driver/director入口、ZStat/Uncommitter std::thread持有 | ZThread派生run_thread/terminate；统一ConcurrentGCThread生命周期 |

## 测试集合处置
- RuntimeWorkers.*九项迁名RelocateWorkers.*，产品对象转generation ZWorkers；转发/完成/并发判据保留。
- GenerationWorkers.CountsAndRunAll→ZWorkers.RunAllUsesMaxWorkersAndRestoresActive；IndependentSets→IndependentGenerationSets；BorrowedTaskJoin/BorrowedCompletionOrder→RunGivesEachActiveWorkerOneDistinctIdBelowActive/CoordinatorReturnsAfterEveryWorkerCompleted。
- GenerationWorkers.PendingAndCycle→PendingRequestSurvivesOrdinaryRunUntilNextCycle；ResizeRestart→RestartableTaskResizesOnRequest；RestartWithoutRequest→RestartableTaskWithoutRequestRunsOnce；BatchStats→RunAccumulatesParallelTimeInStatWorkers及GcDirector.WorkerStatsIncludeInFlightBatch。
- GenerationWorkers.StopJoin/StopCompletionOrder/StopRestartCompletionOrder/OrdinaryRunStopControl检测已删除的Stop三态机，按本包删除清单退休。
- 新增WorkerTaskCapturesGcIdAtConstruction、GenerationCycleOwnsWorkersAndStatWorkers；R3目标改OTHER_VM隔离，stillWaiting及completedAtReturn==3判据保留。
- ZGC gtest无zWorkers单测（本包规格）；本次不声称新增上游移植测试。

Windows遗留GetGCThreadCount导出仍在windows_x86_64_exports.def:2336（既有审查观察；平台导出归#627），不宣称全平台符号已清理。实际函数/持有/调用在Heap范围删除。
