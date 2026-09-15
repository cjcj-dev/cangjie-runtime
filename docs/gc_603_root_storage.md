# #603：根任务与自有根槽

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

坐标：本轮返工起点 `2a65c8132b63c4d7f0d5929ed39d96ce5657fe25`；#596 合入后的主线基线为 `fb7d5282867aaa3b9d8b5df2f6691227ff3424ce`。本文描述实现，独立形态判定由 Review 完成。

## 对应与明确适配

ZGC 根目录：`/root/cj_build/reference/jdk/src/hotspot/share/`。

| 对象 | ZGC 锚 | 本包实现 | 保留的不变量 / 适配事实 |
|---|---|---|---|
| 自有槽块 | `gc/shared/oopStorage.inline.hpp:132-150` | `runtime/src/Common/OopStorage.h:18`：固定槽数组、allocated bitmap、active block array、双向 allocation list | 分配不移动已发布槽；只遍历 allocated 槽；释放后可复用空槽 |
| finalizer weak 登记 | `oops/weakHandle.cpp:39-50` | `FinalizerProcessor.cpp:445`、`Mutator.cpp:177` | Mutator 从真实 processor 的 weak storage 分配；local 队列只持指针，批交不复制槽、不重着色 |
| finalizer strong 根 | `gc/z/zRootsIterator.cpp:159-163` | `FinalizerProcessor.h:82`、`FinalizerProcessor.cpp:250` | queued/running 均由 strongStorage 枚举；队列只负责调度；weak→strong 转交保留当前着色字；终结完成才释放 strong 槽 |
| export 登记 | `gc/shared/oopStorage.inline.hpp:132-150` | `zRootsIterator.hpp:120-237`、`zRootsIterator.cpp:86` | 槽位在 weakStorage；vector 只存 handle generation、占用和 ownership 状态。真实注册与移除 API 不变 |
| old 根任务 | `gc/z/zMark.cpp:797-834` | `zMark.cpp:659-706` | strong colored / strong uncolored iterator，closure 直接调用既有 old producer，worker 结束 FlushMarkStacks；取消 vector\<RootSet\> 中转 |
| young 根任务 | `gc/z/zMark.cpp:852-891` | `zMark.cpp:720-764` | all colored / all uncolored iterator，共用 colored closure；继续调用 #596 MarkYoungGoodBarrierOnOopField，不改代路由 |
| storage-set iterator | `gc/z/zRootsIterator.cpp:159-198` | `zRootsIterator.hpp:22-90`、`zRootsIterator.cpp:363-438` | strong / weak / all 的组合分开；all 按 strong storage、weak storage、外部静态槽 adapter 顺序使用同一个 closure |
| 外部静态槽 ABI adapter | `gc/z/zRootsIterator.cpp:194-198` 中 CLD closure 接口 | `Loader/BinaryFile/CjFile/CjFile.cpp:64` → `zHeap.cpp:258` → `zRootsIterator.cpp:67` | CjFile 发布的是外部槽地址数组；`StaticRootArray::content` 存 NativeSlot*。必须更新原槽，不能复制槽进自有块。原去重、卸载与只读非堆槽合同不改 |
| uncolored 语言根 | `gc/z/zRootsIterator.cpp:166-172` | `RootsIteratorStrongUncolored`、`VisitStrongPlainRoots`、`VisitMinorRootSlots` | Cangjie 使用实际线程栈水位、并发模型根和无头记录；无 HotSpot CLD / nmethod 注册表。保留现有栈扫描器接入，未引入假 CLD/nmethod |
| export 复活/终结生命周期 | `gc/z/zWeakRootsProcessor.cpp:51-73` 是 phantom 清理，非本生命周期的替代 | `zGeneration.cpp:858-889`、`FinalizerProcessor.cpp:250-281` | 本包不引入 phantom 清理；原 resurrection、final enqueue、export ownership 路径保留。value-root 访问改成 visitor 输出，原 RootSet API 的其它调用保留 |

## 已批准但未在本包补齐的形态缺口

我方仍无专用 old `mark_barrier_on_oop_field`，参考当前实读 `gc/z/zBarrier.inline.hpp:591-624`、`gc/z/zBarrier.cpp:146`。按 advisor 的明确范围裁定，`MarkOopClosure` 调现有 ReadStaticRef 和 MarkOldObjectIfActive；本包不发明新 barrier，也不修改 #577 的目标代路由。这个缺口归 P08，不能把整条 old barrier 链宣称为形态闭合。

裁定：
- `/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_603_implement_r5674457578-20260915T035418Z.md`
- `/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_603_implement_r5674457578-20260915T040311Z.md`
- `/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_603_implement_r5674457578-20260915T042600Z.md`

## 删除 / 替换

- finalizer/local 的 `ManagedList<NativeSlot>`：替换为块槽加不拥有槽的 NativeRootHandles。
- export vector 内嵌 `NativeSlot`：替换为 weakStorage 的稳定槽指针。
- old `RootsTask` 的 families 数组与各 worker RootSet 汇总：替换为 old task 内 colored/uncolored iterator 与 producer 发布。
- young task 内单独 uncolored claim flag：由共用 uncolored iterator 持有。
- 未删除弱生命周期、静态根 ABI 更新、#596 young barrier 或 #577 代路由。

## 测试处置

- `RegistrationTransferPreservesSlotAndYoungEpoch`：仅使用真实存储 owner；原地址、着色字和局部队列清空断言保留。
- 新增 `SharedBlockHandlesSurviveGrowthAndTransfer` 和 `ExportBlockGrowthKeepsSlotsAndReleaseSkipsVacancies`：跨 130 个登记槽验证地址、着色字、增长与释放。
- NativeRootCurrent 第二次 old 扫描：观察实际 producer 发布。young 后首次 old 必须发布；old 后重复扫描已标记对象不再发布，依据 `gc/z/zMark.inline.hpp:63-75`。保留当前对象标记、旧地址未标记和槽修复断言。
- PlainMajorRejected：报错断点从已退出 old 根任务的 EnumRefFieldRoot 更新为实际 ReadStaticRef；仍要求 SIGABRT 和精确诊断。
- generation-cycle 的 queued/working 输入改为 strongStorage 槽；两处已删除 API 按 advisor 更新，全部语义断言保留。default SATB companion 的旧装置问题归 #629。

完整证据和机器交付在本棒报告与 `evidence/603/`，它们不替代独立 Review。

- 新增 `NativeRootCurrent.StrongFinalizerRootPublishesAndMarks`：既有 EnqueueFinalizableForTest 种实际 strongStorage 输入，真实 TraceHeap 完整执行后读取 published roots 与 old mark bitmap。没有新增产品导出、friend 或测试 helper；依据 `/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_603_implement_r5674457578-20260915T044224Z.md` 补齐 strong 消费面。

## 完整托管 cycle 的观测限制

同一个 managed wrapper ELF：候选/恢复到达全部 ROOT_FAMILY 断言后出现 `LOADFC current raw value required`（#607 已登记同位点）；基线被更早的 finalizable queue/predicate 检查遮挡。因此这些运行不构成完整托管 cycle 验收通过，也不能据此声称 LOADFC 非本包新增。该对照资格限制与装置问题（#629）在交付报告单列，未提前退出程序或更改断言来取得通过。

## R1 返工：每个 storage 内领取块区段

| ZGC 锚（gc/shared 下） | 本轮实现 | 不变量 / 判定边界 |
|---|---|---|
| `oopStorageSetParState.inline.hpp:38、73` | `zRootsIterator.cpp` 的 strong/weak set 各持每个 storage 的 `ParState<true>` 数组；Apply 对每个 state 调 OopsDo | 每个 worker 都进入每个 storage，领取点不在 storage-set 层 |
| `oopStorage.cpp:1051-1127`、`oopStorageParState.inline.hpp:52-65` | `Common/OopStorage.cpp` BasicParState：activeArray、blockCount、nextBlock、estimatedThreadCount、concurrent；ClaimNextSegment → block Iterate | 按 max_step=10 与 remaining/threads 计算区段，fetch-add 后截断越界；稳定 allocated 集合恰好一次 |
| `oopStorage.inline.hpp:337-347` | `OopStorage::Block::Iterate` | 每块一次读取 atomic bitmap，再调用原 colored closure；释放置 null 先于位图清位 |
| `oopStorage.cpp:1051-1085` | active-array 用 shared_ptr/不可变 vector<Block*> 快照；concurrentIterationCount 禁止块删除 | **基础设施差异，非 ✅**：载体用 shared_ptr/vector 替代自研 ActiveArray；无 HotSpot 分配器/Mutex rank 约束。增长复制指针数组，既有 state 持旧数组与固定 blockCount |
| `oopStorage.cpp:932-988` | Release 与最后一个 BasicParState 析构调用 DeleteEmptyBlocks | **基础设施差异，非 ✅**：无 ServiceThread 与 HotSpot safepoint/锁序约束；清理点为存储操作与末次迭代退出。并发迭代期间禁止删块，末次退出清理空块 |
| `oopStorage.cpp:407-437` | storage mutex 保护分配/释放元数据；registry mutex 只保护 owner 及调度队列 | **基础设施差异，非 ✅**：沿用已裁定 Cangjie mutex 载体；closure 不在 registry/storage mutex 中执行；slot 与 bitmap 均为原子访问 |

基础设施裁定：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_603_implement_r5675303344-20260915T053955Z.md`。主线坐标裁定：同目录 `sym_cangjie_runtime_603_implement_r5675303344-20260915T053852Z.md`。

本轮删除 strong `claimed.exchange` 与 weak storage-family `claimed.fetch_add`，不删除静态 ABI adapter 与语言 uncolored scanner 的独立领取。原 OopStorage 整 activeArray 遍历由 BasicParState/Block 分解替代；FinalizerProcessor/ExportRootTable 的枚举取消外层 registry 锁。

新增 `RootStorageSegments.Strong/WeakFinalizer/Export`：经 TraceHeap / young GC 实际根任务，在第一个 worker 的 post-closure 观察点暂停，另一 worker 完成后释放，读取实际槽结果并断言剩余区段被访问且每槽一次。没有用耗时阈值决定通过。

新增 `RootStorageLifetime.ReleaseAndGrowDuringYoungTask`：真实 young 根任务中增长 export storage，再释放旧块的全部槽，读取 active-array 块数与并发迭代数；验证旧快照不消费后来添加的块、迭代期间保留空块、任务退出后回收空块。观察接口受 MRT_TESTABLE_INTERNALS 控制，算法本体不受宏控制。
