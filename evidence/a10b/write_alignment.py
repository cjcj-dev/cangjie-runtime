from pathlib import Path
import subprocess,json
root=Path.cwd(); z=Path('/root/cj_build/reference/jdk/src/hotspot/share/gc/z')
def line(p,needle):
 for n,s in enumerate(p.read_text().splitlines(),1):
  if needle in s:return n
 raise ValueError((p,needle))
def r(file,needle):return f'{file}:{line(root/file,needle)}'
def zz(file,needle):return f'{file}:{line(z/file,needle)}'
rows=[]
def row(file,needle,zfile,zneedle,scope):rows.append([r('runtime/src/'+file,needle),zz(zfile,zneedle),scope])
row('Heap/z/zDriver.cpp','void CollectorResources::RunDriverLoop','zDriver.cpp','void ZDriverMinor::run_thread','young 循环：receive → driver lock → collect → ack；major 同一循环按独立 port 实例运行，对应 ZDriverMajor::run_thread:463')
row('Heap/z/zDriver.cpp','bool CollectorResources::ProcessDriverRequest','zDriver.cpp','void ZDriverMajor::run_thread','锁覆盖 collect、abort 判断及 ack；old collect 内部短暂释放')
row('Heap/z/zDriverPort.cpp','bool GCDriverPort::Receive','zDriverPort.cpp','ZDriverRequest ZDriverPort::receive','阻塞等待请求；关闭端口唤醒，沿用既有 cooperative abort')
row('Heap/z/zDriver.cpp','bool CollectorResources::ExecuteDriverRequest','zDriver.cpp','void ZDriverMajor::gc','按 cause 选择 full preclean→full roots 或 partial roots，再进入 old；各代 quota 明确指定')
row('Heap/z/zDriver.cpp','void CollectorResources::RunCollection','zGeneration.cpp','void ZGeneration::at_collection_start','计时和 worker 累计从指定 GenerationCycle 取得')
row('Heap/z/zDriver.cpp','void CopyCollector::RunGarbageCollection','zGeneration.cpp','class ZGenerationCollectionScopeYoung','周期前后处理；旧 CopyCollector 层定义移入 zDriver；不再持整周期 STW lock')
row('Heap/z/zDriver.hpp','class DriverLocker','zDriver.cpp','ZDriverLocker::ZDriverLocker','共享 driver 锁 RAII')
row('Heap/z/zDriver.hpp','class DriverUnlocker','zDriver.cpp','ZDriverUnlocker::ZDriverUnlocker','old 主体释放；退出时重取')
row('Heap/z/zGeneration.cpp','void WCollector::DoGarbageCollection','zGeneration.cpp','void ZGenerationOld::collect','old mark/mark-end/non-strong/reset/select/relocate 主体在解锁作用域')
row('Heap/z/zRelocate.cpp','bool WCollector::Preforward','zGeneration.cpp','    ZDriverLocker locker;','young-root remap 与 old relocate-start 同一重取锁区间；remap 在 pause 前')
row('Heap/z/zGeneration.cpp','void WCollector::DoYoungGarbageCollection','zGeneration.cpp','void ZGenerationYoung::collect','young 周期继续持 driver lock；phase 与 worker/stats 显式 YOUNG')
row('Heap/z/zGeneration.cpp','oldCycle.Begin(','zGeneration.cpp','class VM_ZMarkStartYoungAndOld','combined roots 前奏在同一 young pause 建立 old 起点，已有代码保留')
row('Heap/z/zGeneration.hpp','class GenerationCycle','zGeneration.cpp','ZGeneration::ZGeneration','每代持 phase/sequence/reason/active、GCWorkers、GCStats、ZStatCycle')
row('Heap/z/zGeneration.cpp','void GenerationCycle::StartYoungMark','zGeneration.cpp','void ZGenerationYoung::mark_start','young seq 在 mark-start 与 remset flip 一起推进')
row('Heap/z/zGeneration.cpp','void GenerationCycle::Begin','zGeneration.cpp','void ZGenerationOld::mark_start','old seq 在 combined mark-start 的 Begin 推进；young 不在 Begin 推进')
row('Heap/z/zGeneration.cpp','void Collector::PublishGenerationPhase','zGeneration.cpp','void ZGenerationOld::relocate_start','明确代 phase 发布，old relocate-start 捕获 young seq')
row('Heap/z/zGeneration.cpp','void GenerationCycle::PublishPhase','zGeneration.cpp','void ZGeneration::set_phase','指定代 phase 原子发布；沿用我方 pause 子阶段名称')
row('Heap/z/zGeneration.cpp','GCCycleSnapshot GenerationCycle::Snapshot','zGeneration.inline.hpp','inline uint32_t ZGeneration::seqnum','一次读取同一 owner 的周期元数据；不是单 active owner 路由')
row('Heap/z/zGeneration.cpp','void GenerationCycle::SelectReason','zDriver.cpp','void ZDriver::set_gc_cause','原因由指定 generation 保存')
row('Heap/z/zGeneration.cpp','void GenerationCycle::End','zGeneration.cpp','void ZGeneration::at_collection_end','只结束本代 activity')
row('Heap/z/zGeneration.cpp','void GenerationCycle::InitializeWorkers','zGeneration.cpp','ZWorkers* ZGeneration::workers','worker 存储由 generation 所有')
row('Heap/z/zGeneration.cpp','void GenerationCycle::StopWorkers','zCollectedHeap.cpp','void ZCollectedHeap::stop','停止协议的资源端：driver join 后释放 generation 所有 worker；ZGC 对应 stop/abort 调用层')
row('Heap/z/zCollectedHeap.hpp','virtual GenerationCycle& GetGenerationCycle','zGeneration.inline.hpp','inline ZGenerationYoung* ZGeneration::young','mutable/const owner 查询与 phase/snapshot/stats 入口')
row('Heap/Collector/CollectorProxy.h','GenerationCycle& GetGenerationCycle','zGeneration.inline.hpp','inline ZGenerationOld* ZGeneration::old','所有代理读写都到真实 collector 的 generation，包括 remset seq 比较')
row('Heap/Collector/CollectorProxy.cpp','void CollectorProxy::RunGarbageCollection','zGeneration.inline.hpp','inline ZGenerationYoung* ZGeneration::young','请求不再重复写共享 currentCollector；只调用初始化时绑定的对象')
row('Heap/z/zDriver.cpp','GCWorkers& CollectorResources::GetWorkers','zGeneration.cpp','ZWorkers* ZGeneration::workers','按显式 generation 取得 owner 的 worker')
row('Heap/z/zDriver.cpp','GCStats& CollectorResources::GetGCStats','zGeneration.cpp','ZGeneration::ZGeneration','旧统计访问兼容静态 old 默认；young 调用显式选择 YOUNG，不存在 active 路由')
row('Heap/z/zDriver.cpp','bool CollectorResources::IsGcStarted','zDriver.cpp','bool ZDriverMinor::is_busy','只读两代 activity 聚合用于堆活动查询，不参与选择代')
row('Heap/z/zDriver.cpp','void CollectorResources::StopGCWork','zDriver.cpp','void ZDriverMajor::terminate','分别关闭两个端口，再 join 两个 driver，沿用 abort 清理')
row('Heap/z/zDriver.cpp','void CollectorResources::StartGCThreads','zDriver.cpp','ZDriverMinor::ZDriverMinor','各自线程与 worker owner 初始化')
row('Heap/z/zDirector.cpp','void CollectorResources::EvaluateDirector','zGeneration.cpp','void ZGeneration::set_active_workers','采样/resize 都到明确代的 CycleStats/Workers；director 规则本体保留')
row('Heap/z/zGeneration.cpp','void TracingCollector::PreGarbageCollection','zGeneration.cpp','void ZGeneration::at_collection_start','phase/reason/workers/stats 接口传代')
row('Heap/z/zGeneration.cpp','void TracingCollector::PostGarbageCollection','zGeneration.cpp','void ZGeneration::at_collection_end','只结束该代 worker；phase 握手显式选择代')
row('Heap/z/zForwarding.hpp','static bool young_marking','zGeneration.inline.hpp','inline bool ZGeneration::is_phase_mark','读 young owner；ENUM/CLEAR/TRACE 为已有 Mark 子阶段')
row('Heap/z/zHeap.cpp','GCPhase HeapImpl::GetGCPhase','zGeneration.inline.hpp','inline bool ZGeneration::is_phase_mark','Heap/Collector GetGCPhase 与 SetGCPhase 均强制 generation 参数')
row('Mutator/MutatorManager.cpp','void MutatorManager::TransitionAllMutatorsToGCPhase','zGeneration.cpp','class VM_ZYoungOperation','将 generation 传入现有 mutator 握手；STW lock 只包相位操作')
row('Mutator/MutatorManager.cpp','void MutatorManager::StartLightSync','zGeneration.cpp','void ZGenerationOld::pause_relocate_start','old pause 明确 OLD，并设置该次 mutator 操作的 EnumYoung=false')
row('Mutator/MutatorManager.cpp','void MutatorManager::ExcludeNewMutatorFromActiveEpoch','zBarrierSet.cpp','void ZBarrierSet::on_thread_attach','新加入线程继承现有 root operation 的 generation；不从某个 active cycle 推断')
row('Mutator/Mutator.cpp','bool Mutator::AcknowledgeEpochHandshake','zGeneration.cpp','class VM_ZYoungOperation','ack 从本次操作的 EnumYoung 查询正确代 phase')
row('Mutator/Mutator.inline.h','bool Mutator::TransitionGCPhase','zGeneration.cpp','class VM_ZYoungOperation','phase 转换消费者同样读取操作指定的 generation')
row('Heap/Collector/Collector.cpp','Generation Collector::ObjectGeneration','zPage.inline.hpp','inline ZGenerationId ZPage::generation_id','对象相关消费者按页 owner；删除 active forwarding generation')
row('Heap/z/zStoreBarrierBuffer.cpp','MAddress RemapPendingField','zStoreBarrierBuffer.cpp','void ZStoreBarrierBuffer::on_new_phase_relocate','pending holder 读取页 owner 的 relocate phase；buffer 算法保留')
row('Heap/z/zThreadLocalAllocBuffer.cpp','MAddress AllocBuffer::Allocate','zGeneration.inline.hpp','inline bool ZGeneration::is_phase_mark','删除 heap/global mutator TRACE 到 young marking 的桥；读取分配页 owner')
row('Heap/z/zRelocate.cpp','void WCollector::StartRelocationTasks','zGeneration.cpp','void ZGenerationOld::relocate_start','young/old 调用点显式传 generation，不再从原因猜测')
row('Heap/Collector/CopyCollector.cpp','void CopyCollector::ForwardFromSpace','zGeneration.cpp','void ZGenerationOld::concurrent_relocate','共享执行器携带 generation；明确选择对应 worker')
row('Heap/z/zRelocate.cpp','void RegionManager::FinishIncompleteFromRegions','zGeneration.cpp','void ZGenerationOld::concurrent_relocate','清理筛选由传入 generation 决定，不读取共享统计 reason')
row('Heap/z/zDriver.cpp','void CollectorResources::RunYoungCollection','zGeneration.cpp','class ZGenerationCollectionScopeYoung','young type RAII 包围整个年轻代周期')
row('Heap/z/zDriver.cpp','bool CollectorResources::ShouldPrecleanYoung','zDriver.cpp','static bool should_preclean_young','显式 full 原因或旧代分配等待时 preclean；其余原因 partial roots')
row('Heap/z/zGeneration.cpp','YoungTypeSetter::YoungTypeSetter','zGeneration.cpp','ZYoungTypeSetter::ZYoungTypeSetter','类型 none→指定类型→none，作用域结束恢复')
row('Heap/z/zGeneration.cpp','void GenerationCycle::SelectTenuringThreshold','zGeneration.cpp','void ZGenerationYoung::select_tenuring_threshold','full preclean 阈值 0；其余类型沿用计算算法')
row('Heap/z/zDriver.cpp','GCDriverPort& CollectorResources::GetYoungDriverPort','zGeneration.cpp','class VM_ZYoungOperation','根据 young type 选择 minor 或 major 的操作上下文')
row('Heap/z/zMark.cpp','if (GetGenerationCycle(GCCycleGeneration::YOUNG).IsMajorRoots())','zGeneration.cpp','void ZGenerationYoung::mark_roots','只有 major roots 发布 old 根；preclean 不提前开始 old mark')
# These rows cover changed owner arguments inside otherwise out-of-scope component bodies.
for file,needle in [
 ('Heap/z/zMark.cpp','void WCollector::TraceHeap'),('Heap/z/zMark.cpp','void WCollector::StartYoungMarkWork'),
 ('Heap/z/zMark.cpp','bool WCollector::FollowYoungMark'),('Heap/z/zMark.cpp','size_t TracingCollector::RunMajorStripeMark'),
 ('Heap/z/zMark.cpp','void TracingCollector::TracingImpl'),('Heap/z/zRootsIterator.cpp','void TracingCollector::DoEnumeration'),
 ('Heap/z/zRelocate.cpp','void WCollector::EvacuateYoungRegions'),('Heap/z/zRelocate.cpp','BaseObject* WCollector::TryMutatorRelocate'),
 ('Heap/z/zRelocate.cpp','BaseObject* WCollector::TryForwardObject'),('Heap/z/zRelocate.cpp','BaseObject* WCollector::ForwardObjectImpl'),
 ('Heap/z/zRelocate.cpp','void RegionManager::CompactRegion')]:
 try:row(file,needle,'zGeneration.inline.hpp','inline ZGenerationYoung* ZGeneration::young','本包仅迁移 generation/worker/stats 参数；标记/搬移本体不作新实现或独立等价声明')
 except ValueError:pass
text='''待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# A10b：双 driver 与 generation owner

本轮角色 implement；形态对齐，不含 gate/unit/切刀执行或并发运行验收。
冻结基线 `3c3216a7293b28690411a6283c7fd74076bdcd1b`；已按内容合入主线 `78fc9ce028de705b3ea705b7069759c1036a2796`。
R 根：`'''+str(root)+'''/`；Z 根：`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`。
表中是本包修改的机制或调用参数对应，不把未改的外层标记、搬移、互操作及诊断函数声明成逐指令移植。

## 函数对应表

| R file:line | Z file:line | 本包范围 |
|---|---|---|
'''
text+='\n'.join('| '+ ' | '.join(row)+' |' for row in rows)+'\n'
text+='''
Heap dump 例外由 advisor 明确批准：`Heap::DumpHeap`（R `runtime/src/Heap/z/zHeap.cpp`）与三个请求线程调用点对应 `/root/cj_build/reference/jdk/src/hotspot/share/services/heapDumper.cpp:2640` 的 `VM_HeapDumper::doit`、`:2883` 的 VMThread 调用层。我方没有 VMThread，沿用 `CjHeapData::DumpHeap`（R `runtime/src/Inspector/CjHeapData.cpp:93`）及 IDE `Serialize`（R `runtime/src/Inspector/HeapSnapshotJsonSerializer.cpp:22`）内部已有 STW；driver 不再消费 Inspector 队列。

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
两份精确集合差（相对冻结基线及当前主线）见 `evidence/a10b/test-set-diff.json`。不运行测试门，测试执行状态为 NOT_RUN(alignment_mode)。

## 构建与限制

只使用规定 `kkk2_build_two.sh`，default/testable 两臂同时构建，`-j$(nproc)`=192。原样 rc、wall 和 SO 摘要见 `evidence/a10b/final-validated-build.log`；远端完整日志在 `kkk2:/root/sym_cangjie_runtime_494_implement_r5655389950/`。
较早 `driver-build-2.log` 是合并尚未解决冲突时的无效构建尝试，不用于候选结论；最终构建发生在解决冲突之后。中间编译错误保留于 ownership-build*.log；没有故意破坏或运行 gate/unit。
不声明并发运行、性能、Windows/AArch64 构建验收；导出登记及 AArch64 调用点随接口同批更新。

## FALSIFIED

冻结 `3c3216a7293b28690411a6283c7fd74076bdcd1b:runtime/src/Heap/z/zPage.cpp:300` 的 `GetSnapshotEpoch` 已经按页所属 generation 读取 sequence，旧包书的“页 snapshotEpoch 代替 generation seq”不是当前缺口。本包没有重复创建计数。
冻结源码的 `ZForwarding::young_marking` 已经查询 young snapshot，本包删除的 global TRACE 桥在分配路径，不能把旧描述直接归到 forwarding getter。
'''
(root/'docs/a10b_driver_generation.md').write_text(text)
(root/'evidence/a10b/function-map.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n')
print('function-map rows',len(rows))
