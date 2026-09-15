# #603：根任务与自有根槽

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

坐标：任务冻结 `c3973505c171aa7e72095c3357ffd57f56156707`；按本条“消费 #596”要求，实际实现基线为合入后的 `fb7d5282867aaa3b9d8b5df2f6691227ff3424ce`。本文描述实现，独立形态判定由 Review 完成。

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
