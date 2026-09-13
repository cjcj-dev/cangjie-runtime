> R2 修正（候选产品 9553f311dabf537273a93d21ee2601b80c5a8275）：原报告的 PromotionPage 遍历对应漏掉 ZLiveMap::is_marked 序号条件。当前 zPage.cpp:195–198 在读取位图前比较原年轻代**当前**序号，不使用晋升后槽位的老年代身份，也不保存 clone 时的序号。详情及本轮新构建结果见 `evidence/d10-r2/` 与本棒报告。下文既有构建数字仅是首轮历史证据。

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# D10：命名根启发式与 retained 副本删除

坐标基于产品提交 `c93c0b9d95252f3c7d6fc5053c4435aea7fdc7d3`。冻结 `78fc9ce028de705b3ea705b7069759c1036a2796`；交付前合入主线 `403916767341fd0610fdc7a1201f1a0333e0da38`（#494 / PR #505）。文中我方相对路径根为 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_503_implement_r5656150642`；ZGC 根为 `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`，表内 `compiler/oopMap.cpp` 是 reference/jdk/src/hotspot/share 下的路径。

本轮 alignment_mode：交形态与构建结果，不运行 gate/unit/切刀/nwdet。构建内置静态检查正常执行，未修改任何检查器或 known_failures。没有运行时验收结论。

## ZGC 函数对应表

| 我方函数/机制 | 我方锚 | ZGC 锚 | 本包结果 |
|---|---|---|---|
| MarkObjectImpl / MarkEntryObject / ResurrectObject | runtime/src/Heap/z/zMark.cpp:63 | zMark.cpp:394-428; zPage.inline.hpp:284 | 删除对象头猜测及返回“已标记”的丢弃路径；直接消费对象起始地址与页 livemap。 |
| 根及字段入口 | runtime/src/Heap/z/zMark.cpp:104 | zMark.cpp:669-692; zMark.cpp:300 | 删除三枚命名根启发式及回扫；保留明确的 null/堆域归约，理由见基础设施表。 |
| Mutator 枚举 / 派生根 | runtime/src/Mutator/Mutator.cpp:872 | zMark.cpp:669-692; compiler/oopMap.cpp:923-955 | 堆根直接发布；派生指针按 metadata 给的 base + offset 重建，不再推测另一个 base。 |
| ForwardObject / RelocateObjectInner | runtime/src/Heap/z/zRelocate.cpp:1545 | zRelocate.cpp:382-415 | 删除头启发式分支及其内部 recovery；已有 find→retain→copy→insert 链继续消费对象起始地址。 |
| CloneForPromotion / PromotionPage | runtime/src/Heap/z/zPage.cpp:175 | zPage.cpp:64-71 | 原 young 页的 LiveInfo 本体移出 region 槽位；old 槽位装新 map；不复制位图。 |
| TakePageLiveInfo | runtime/src/Heap/Collector/LiveInfoArena.h:51 | zPage.cpp:64-71; zRelocationSet.cpp:191-209 | 仅把原 map 的释放责任从槽位登记表移给原页对象；所有权转移，不是第二份快照。 |
| Add / ResetFlipPromotedPages | runtime/src/Heap/z/zRelocationSet.cpp:102 | zRelocationSet.cpp:191-209 | relocation set 持有原 young 页，年轻代原 forwarding reset 后释放；generation 调用在 zGeneration.cpp:556。 |
| RememberFlipPromotedPages | runtime/src/Heap/z/zRelocate.cpp:1143 | zRelocate.cpp:1257-1306 | worker 对原页 ObjectIterate；回调由 task 按值持有至 worker join。 |
| PromotionPage::ObjectIterate / VisitLiveObjectsUntilFalse | runtime/src/Heap/z/zPage.cpp:190 | zPage.inline.hpp:320-331 | 只遍历普通 livemap 起始位；删除头过滤、密集大小步进和失败后剩余位猜测。 |
| ForEachLiveObjectStart | runtime/src/Heap/z/zRelocate.cpp:1816 | zPage.inline.hpp:320-331 | 重定位页 walk 从普通 live 起始位发起，再访问对象头。 |
| CollectLargeGarbage | runtime/src/Heap/z/zPageAllocator.cpp:851 | zGeneration.cpp:206-229; zPage.inline.hpp:320-331 | 存活大页保留原 livemap；删掉依赖副本的提前 ResetMarkBit 及 recent-large 清位 walk。 |
| SlotHeldByLiveObject | runtime/src/Heap/z/zRemembered.cpp:90 | zPage.inline.hpp:371-391; zRemembered.cpp:50-76 | 用既有 FindLiveObjectStart 查所属对象并检查字段在对象尺寸内；不回扫猜 TypeInfo。 |
| ScrubMinorFreeTarget | runtime/src/Heap/z/zMark.cpp:805 | zRemembered.cpp:81-149 | 删 retained OR current-minor-root 放行策略及全链输入；原 free/garbage 处理依 advisor 保持。 |
| ZForwarding::verify → RegionInfo::VerifyLive | runtime/src/Heap/z/zForwarding.cpp:294 | zForwarding.cpp:369-406; zPage.cpp:196-203 | 把已有精确对象数/字节数比較下沉页级；保留一条校验链，各指标独立断言。 |
| TracingCollector 非 WCollector 标记入口 | runtime/src/Heap/z/zMark.inline.hpp:11; zMark.hpp:380; zCollectedHeap.cpp:35 | zMark.cpp:394-428; zPage.inline.hpp:284 | 同批删除三处命名过滤/恢复，不保留另一份入口路径。 |
| allocator header 消费点 | runtime/src/Heap/Allocator/SlotList.h:40,58; zPageAllocator.hpp:439; zPageAllocator.cpp:803 | ZGC 无头启发式；zPage.inline.hpp:320 | 删除过滤调用；free-slot 容器本身不属于本包根启发式/retained 机制。 |
| 对象写入诊断 / TypeInfo 注释 / HealSite 枚举 | runtime/src/Common/BaseObject.cpp:22; ObjectModel/RefField.h:39 | ZGC 无推测 holder 的诊断回扫 | 删除诊断回扫及失去调用的九个写入归属枚举；保留实际写入检查。 |
| 遗留工具脚本 | 冻结 tools/trustp1_static_harness.sh:1 | z_globals.hpp:30（无对应） | 删除依赖已删 MRT_GCV2 开关的唯一仓内 tools 脚本；两臂清单见 legacy-script-scan.json。 |

## 基础设施差异与范围

| 保留项 | 差异事实与实读锚 | ZGC 对照 | 边界 |
|---|---|---|---|
| 堆地址域归约 | Mutator.cpp:793-833、:873-894：Cangjie 栈对象包含 TypeInfo，struct-live 可给出无头 ABI record（String 的首字为引用）；栈对象的字段继续作为根枚举。zMark.cpp:114、:142、:175：静态/ABI 根可含非堆目标。 | zMark.cpp:669-692 通过 oop-slot closure 消费 heap oop；compiler/oopMap.cpp:923-955 的 base/derived 配对。 | 只按存储/地址域归约，不按堆对象头的数值阈值/TypeInfo residence/有限回扫来丢弃堆根。 |
| 明确派生根 | Mutator.cpp:1083 MakeDerivedRootVisitor 使用 metadata 给定 base，保持 derived offset。test_remset.cpp:119 显式 knownBase 仍在；普通、tagged、moving base 的产品测试保持。 | ZGC oop-map 派生指针与 base 配对；zMark.cpp:691。 | 删除把已经是 interior 的“base”再次靠回扫恢复的测试；不把真 base/derived 合法配对误删。 |
| region 槽位复用 | zPage.cpp:175、zPage.hpp 的 PromotionPage；原 map 从 LiveInfoArena 登记表移交，member unique_ptr 单独拥有。 | zPage.cpp:64 clone_for_promotion 新建 old page，原 young page 留给 relocation set。 | 不改页表映射或 UnitInfo 数组布局；不复制位图，不跨新周期回读。advisor-2.md 明确授权此表示适配。 |
| 校验读取原页 | zPage.cpp:214：通过现有 FromPageView 访问原普通 livemap，in-place 用该原页视图。 | zForwarding.cpp:406 调原 ZPage::verify_live。 | 复用已有原页拥有关系，未创建第二份计数或第二条验证链。 |

两处未纳入本次命名机制删除的直接对象头校验：EnsureRouteDomainMembership 与 FixMinorObjectSlots 内的 IsValidObject 条件。曾提出同批删除，自动审批将其判为命名启发式之外的独立安全边界并拒绝提交。额外修改已恢复，diff rc=0；通过结构化 new_issues 请求另行核定对应/授权。保留不是以“有用”为理由作 ZGC 等价裁决，亦不声称这两处已完成对齐。原文见 evidence/d10/approval-boundary.txt。

## producer → consumer 与原页生命周期

1. 根：Mutator::GcPhaseEnum → PushHeapRoot → PublishThreadRoot → MarkEntryObject；显式派生根先处理 base 再按 offset 回写。命名过滤/回扫已从枚举、mark、relocate、页遍历及导出/测试删掉。
2. flip promotion：AddFlipPromotedPage → CloneForPromotion → TakePageLiveInfo（移交原 ordinary LiveInfo）→ PromoteYoungRegion（region 槽位装 old 新 map）→ RememberFlipPromotedPages::PageTask::Work → PromotionPage::ObjectIterate。原页仍由 flipPromotedPages 拥有，年轻代下一次 ForwardingTable::ResetRelocationSet 后 ResetFlipPromotedPages 才释放。forwarding 的旧原页借用读者在此之前仍可读同一 map。
3. 存活数：ZForwarding::verify 遍历已填充 entries，累计目标对象尺寸与对象数 → RegionInfo::VerifyLive 对原页 ordinary bitmap 的两项计数逐项比较。两项分别对应独立诊断。
4. unselected 大页：CollectLargeGarbage 的 survivor 分支保留 ordinary map；不再靠提前 ResetMarkBit 加 retained 副本保活。

## 删除清单

逐符号 `git grep -n -F <symbol> <ref> -- runtime/src` 的 stdout/stderr/rc 完整原文见 `evidence/d10/deletion-scan.txt`，可用 `python3 evidence/d10/capture.py` 重跑。冻结/候选使用同一作用域同一把尺；冻结命中就是每项的阳性对照。下表的冻结锚是该符号实际命中的首行，完整调用与声明均在原文中。

| 删除符号/字段/入口 | 冻结命中锚 | 冻结 rc | 候选 rc |
|---|---|---:|---:|
| `MarkGoodHeapGate` | `runtime/src/Heap/Collector/Collector.cpp:238` | 0 | 1 |
| `PlausibleManagedObjectGate` | `runtime/src/Heap/Allocator/SlotList.h:43` | 0 | 1 |
| `TryRecoverInteriorBase` | `runtime/src/Common/BaseObject.cpp:30` | 0 | 1 |
| `ManagedObjectGate.h` | `runtime/src/Heap/Allocator/SlotList.h:12` | 0 | 1 |
| `kMinPlausibleTypeInfoAddr` | `runtime/src/Heap/Collector/Collector.cpp:116` | 0 | 1 |
| `TipLow32IsZero` | `runtime/src/Heap/Collector/Collector.cpp:123` | 0 | 1 |
| `ObjectFitsInRegion` | `runtime/src/Heap/Collector/Collector.cpp:128` | 0 | 1 |
| `TipWordLooksLikeTypeInfo` | `runtime/src/Heap/Collector/Collector.cpp:149` | 0 | 1 |
| `ClassifyInteriorOffset` | `runtime/src/Heap/Collector/Collector.cpp:171` | 0 | 1 |
| `RecoverInteriorBaseImpl` | `runtime/src/Heap/Collector/Collector.cpp:208` | 0 | 1 |
| `ToHeaderCovered` | `runtime/src/Heap/WCollector/WCollector.h:575` | 0 | 1 |
| `PushHeapRootIfPlausible` | `runtime/src/Mutator/Mutator.cpp:872` | 0 | 1 |
| `RetainedLiveInfoState` | `runtime/src/Heap/z/zPage.hpp:70` | 0 | 1 |
| `IsRetainedLifeCurrent` | `runtime/src/Heap/z/zPage.hpp:242` | 0 | 1 |
| `GetRetainedLiveInfo` | `runtime/src/Heap/z/zPage.hpp:244` | 0 | 1 |
| `HasEverPreservedRetainedLiveInfo` | `runtime/src/Heap/z/zPage.hpp:251` | 0 | 1 |
| `GetRetainedLiveInfoEpoch` | `runtime/src/Heap/z/zPage.hpp:253` | 0 | 1 |
| `GetRetainedLiveInfoCoveredUpTo` | `runtime/src/Heap/z/zPage.hpp:258` | 0 | 1 |
| `StampRetainedSnapshot` | `runtime/src/Heap/z/zPage.hpp:263` | 0 | 1 |
| `RetainedOwnCopyEnabled` | `runtime/src/Heap/z/zPage.hpp:273` | 0 | 1 |
| `CaptureRetainedMarkWords` | `runtime/src/Heap/z/zPage.hpp:277` | 0 | 1 |
| `HasRetainedMarkWords` | `runtime/src/Heap/z/zPage.hpp:279` | 0 | 1 |
| `RetainedMarkWordsSay` | `runtime/src/Heap/z/zPage.hpp:285` | 0 | 1 |
| `FreeRetainedMarkWords` | `runtime/src/Heap/z/zPage.hpp:287` | 0 | 1 |
| `BeginRetainedPreserve` | `runtime/src/Heap/z/zPage.hpp:295` | 0 | 1 |
| `PreserveRetainedLiveInfo` | `runtime/src/Heap/z/zObjectAllocator.cpp:370` | 0 | 1 |
| `PreserveRetainedLiveInfoUpTo` | `runtime/src/Heap/z/zObjectAllocator.cpp:370` | 0 | 1 |
| `NoteRetainedPreserve` | `runtime/src/Heap/z/zPage.hpp:294` | 0 | 1 |
| `IsRetainedSnapshotValid` | `runtime/src/Heap/z/zPage.hpp:320` | 0 | 1 |
| `FORWARDING_FACE_RESET_BIT` | `runtime/src/Heap/z/zPage.hpp:752` | 0 | 1 |
| `IsForwardingFaceReset` | `runtime/src/Heap/z/zPage.hpp:754` | 0 | 1 |
| `SetForwardingFaceReset` | `runtime/src/Heap/z/zPage.hpp:756` | 0 | 1 |
| `ClearForwardingFaceReset` | `runtime/src/Heap/z/zPage.hpp:761` | 0 | 1 |
| `retainedLiveInfo` | `runtime/src/Heap/z/zPage.hpp:246` | 0 | 1 |
| `retainedEverPreserved` | `runtime/src/Heap/z/zPage.hpp:251` | 0 | 1 |
| `retainedLiveInfoEpoch` | `runtime/src/Heap/z/zPage.hpp:255` | 0 | 1 |
| `retainedLiveInfoCoveredUpTo` | `runtime/src/Heap/z/zPage.hpp:260` | 0 | 1 |
| `retainedLifeId` | `runtime/src/Heap/z/zPage.hpp:297` | 0 | 1 |
| `retainedPreserveCnt` | `runtime/src/Heap/z/zPage.hpp:758` | 0 | 1 |
| `retainedMarkWords` | `runtime/src/Heap/z/zPage.hpp:281` | 0 | 1 |
| `retainedMarkWordCnt` | `runtime/src/Heap/z/zPage.hpp:1061` | 0 | 1 |
| `IsForwardingFaceCurrent` | `runtime/src/Heap/z/zPage.hpp:750` | 0 | 1 |
| `ResetMarkBit` | `runtime/src/Heap/z/zPage.hpp:341` | 0 | 1 |
| `KeepRememberedHolder` | `runtime/src/Heap/WCollector/RememberedHolderPolicy.h:12` | 0 | 1 |
| `RememberedHolderPolicy.h` | `runtime/src/Heap/Collector/Mark.cpp:10` | 0 | 1 |
| `holderIsCurrentMinorRoot` | `runtime/src/Heap/WCollector/WCollector.h:900` | 0 | 1 |
| `preservedByCurrentRoot` | `runtime/src/Heap/WCollector/WCollector.h:901` | 0 | 1 |
| `currentRootRanges` | `runtime/src/Heap/z/zRelocate.cpp:1100` | 0 | 1 |
| `g_fixinputReject` | `runtime/src/Heap/z/zRelocate.cpp:1029` | 0 | 1 |
| `g_fixinputRecover` | `runtime/src/Heap/z/zRelocate.cpp:1030` | 0 | 1 |
| `g_fixinputUnrecoverable` | `runtime/src/Heap/z/zRelocate.cpp:1031` | 0 | 1 |

冻结前已删：retained 低位递增与 probe 计数。冻结 zPage.hpp:1052 明写仅使用 FORWARDING_FACE_RESET_BIT，故本包认领的是剩余高位标志、字段及全部 retained 载体的删除，不认领此前已删的低位递增。

另删 `tools/trustp1_static_harness.sh`、九个失去产品调用的 HealSite 枚举项及 retained 专属测试绑定清单/runner 校验段；`runtime/tests/check_heal_slot_writes.py` 未改。保留独立 free-slot colour 元数据测试。

## 测试同批移植及集合差

本包相对合入主线只新增两项：

- ZLiveMapPort.CloneForPromotionKeepsOriginalPageLivemap（test_live_map.cpp:641）：对应 ZPage::clone_for_promotion / object_iterate；大页分支对应 ZLiveMapTest.strongly_live_for_large_zpage。参考没有独立同名 clone 测试，不虚称同名移植。
- ZLiveMapPort.LiveIteratorVisitsOnlyObjectStarts（test_live_map.cpp:679）：对应 ZPage::object_iterate 的起始位不变量；含终止回调分支。

修改 ZVerify.ForwardingTableChecksLiveAccounting（test_verify_fail_close.cpp:102）：沿用 ZForwardingTest.find_full / zForwarding.cpp:369-406 的产品链，分别给对象数和字节数断言；没有把两种故障合并成一个失败条件。

以上测试源码随产品交付，按 alignment_mode **未编译测试 ELF、未运行测试、未做故意破坏转红**。构建 runtime 不能借作测试执行证据。

本包删除的测试如下，理由为对应旧机制整体删除，绝非为改变门结果。完整冻结到候选的集合差另见 test-set-diff.json（包含 #494 主线测试变更）；本包净差见 package-test-set-diff.json。

| 删除测试 | 删除理由 |
|---|---|
| DefectRegress.MarkGoodHeapGateBlocksNonHeap | 对象头猜测/有限回扫机制删除 |
| DefectRegress.TipSmallIntRejectThenRecoverHost | 对象头猜测/有限回扫机制删除 |
| DefectRegress.TipSmallIntRejectWithoutRecoverIsLoss_Contract | 对象头猜测/有限回扫机制删除 |
| ForwardingPublicationProduct.FixRootInteriorFailClosedWhenHostUnresolved | 非法 interior-as-base 依赖启发式恢复；显式合法 base/derived 配对测试保持 |
| ForwardingPublicationProduct.PreForwardDerivedInteriorBaseProducer | 非法 interior-as-base 依赖启发式恢复；显式合法 base/derived 配对测试保持 |
| ForwardingPublicationProduct.PreForwardDerivedInteriorMovingBaseProducer | 非法 interior-as-base 依赖启发式恢复；显式合法 base/derived 配对测试保持 |
| ForwardingPublicationProduct.PreForwardDerivedInteriorUnrecoveredHostFailsClosed | 非法 interior-as-base 依赖启发式恢复；显式合法 base/derived 配对测试保持 |
| LiveMap.ExaminedPageWithoutSnapshotStillAborts | retained 副本/覆盖策略删除 |
| LiveMap.OwnedCopyExaminedPageWithoutSnapshotStillAborts | retained 副本/覆盖策略删除 |
| LiveMap.RetainedCaptureKeepsCurrentYoungFaceAfterStaleForwardingDone | retained 副本/覆盖策略删除 |
| LiveMap.RetainedCaptureRejectsOldFromFaceWhenNewCycleMarksNothing | retained 副本/覆盖策略删除 |
| LiveMap.RetainedCaptureSkipsYoungFaceAfterForwardingDone | retained 副本/覆盖策略删除 |
| LiveMap.RetainedCaptureUnionsYoungFaceBeforePromotion | retained 副本/覆盖策略删除 |
| LiveMap.RetainedCaptureUnionsYoungLargeFlagBeforePromotion | retained 副本/覆盖策略删除 |
| LiveMap.RetainedLargeMarkBitSurvivesCurrentFaceLoss | retained 副本/覆盖策略删除 |
| LiveMap.RetainedMarkWordsSurviveUnbindAndMapReset | retained 副本/覆盖策略删除 |
| LiveMap.UnexaminedRelocselPageKeepsWithoutSnapshot | retained 副本/覆盖策略删除 |
| ObjectGate.FreePinnedPushFrontRejectsFreeRegion | 对象头猜测/有限回扫机制删除 |
| ObjectGate.GeomCrossEndRejectedOnActiveRegion | 对象头猜测/有限回扫机制删除 |
| ObjectGate.HeaderConsumersRejectFreeRegion | 对象头猜测/有限回扫机制删除 |
| ObjectGate.NonInteriorNoFalseRecover | 对象头猜测/有限回扫机制删除 |
| ObjectGate.PlausibleTipAccepted | 对象头猜测/有限回扫机制删除 |
| ObjectGate.RawArrayPlus8RecoversBase | 对象头猜测/有限回扫机制删除 |
| ObjectGate.TipInHeapRejected | 对象头猜测/有限回扫机制删除 |
| ObjectGate.TipSmallIntRejected | 对象头猜测/有限回扫机制删除 |
| Remset.CurrentMinorRootOverridesRetainedDeadSnapshotAndNullHeal | retained 副本/覆盖策略删除 |

## 双构型构建及产物身份

唯一入口 `/root/cj_build/ops/bin/kkk2_build_two.sh <本工作树> <lane>`。default/testable 并行两臂，每臂 `-j$(nproc)=192`，三个 compiler launcher 均 ccache。只构建，不跑运行时门；CPU affinity 观测 0-191。没有负载 ELF，无性能/运行时测量结论。

| 构型 | cmake | configure rc | build rc | wall | runtime SO sha256 |
|---|---|---:|---:|---:|---|
| default | -DMRT_TESTABLE_INTERNALS=OFF | 0 | 0 | 62s | 0b1639adda9cc8941049d8886d8ed6abe31f934ecebcb206f6a5078d99f5ffa5 |
| testable | -DMRT_TESTABLE_INTERNALS=ON | 0 | 0 | 61s | a469eba900fca2c69a09cdeeb33b8985a292ef9761ae7dc966114c71dc553b90 |

两臂 boundscheck SO sha256 均为 `f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883`；runtime SO 与 boundscheck SO 同臂同次构建。源码 tar SHA256 `1a4bd74e3ea74d8a5f63b875e6660eaa3955d14e189b2881d1c6ff2da12284f8`；CMake 的 CJ_RUNTIME_COMMIT 是产品提交 c93c0b9d95252f3c7d6fc5053c4435aea7fdc7d3。dirty_files=1 仅为未登记的 evidence/d10，不含产品修改；product-files.sha256 可独立复算内容。

两端 uptime：05:32:08 up 4 days, 5:40，load 0.97/3.11/2.61；05:33:10 up 4 days, 5:41，load 5.15/3.77/2.86。原文、构型、产物绝对路径见 artifact-metadata.txt。最终日志 kkk2:/root/sym_cangjie_runtime_503_implement_r5656150642/{default,testable}-{configure,build}.log。上一中间态成功日志归档在 history/owned-page/；首次失败仅保留本地原结果摘要及定点失败原文（远端首次日志被后续构建覆盖，不虚称仍在）。

阳性对照：首次两臂 build rc=2，check_heal_slot_writes 报 9 个没有产品调用的 HealSite 枚举项。随后删除枚举而未改检查器，最后 build rc=0。此证据仅说明构建检查能拒绝未清理枚举，不是切刀或运行时行为证明。

## 主线内容核

交付前 fetch + merge cjcjdev/main rc=0，Already up to date。主线 #494 包的七个定义特征逐项 git grep -c：两臂均有命中，候选计数不少于主线；stdout/rc 在 main-content.json。这只是内容保存证据，不是自审放行。

## 六栏验收卡（alignment_mode）

| 栏 | 本轮证据/限制 |
|---|---|
| 基线身份及摘要 | coordinate.txt；冻结实读 rc=0；最终产品文件摘要 product-files.sha256；source.tar.gz 摘要见上。 |
| 真实产品调用链 | 上述 producer→consumer 表与 ZGC 双侧锚；删除消费者原文在 deletion-scan.txt。 |
| 测试/产品归属 | 源码测试调用 CloneForPromotion、VisitLiveObjectsUntilFalse、ZForwarding::verify；未构建测试 ELF，未主张 nm/运行证据。 |
| 产物身份 | 两臂 runtime/boundscheck SO SHA256、CMake source commit、源码 archive SHA256，见 artifact-metadata.txt。 |
| 定向切刀 | alignment_mode 不执行；首次构建失败不是定向切刀。 |
| 测试集合差 | 冻结到候选与本包相对合入主线两份机械集合差；新测试未运行。 |

## FALSIFIED

1. #503 追加项称我方没有精确 live_objects/live_bytes 比较：冻结 zForwarding.cpp:276-304 已有计数、去重、尺寸累加和精确比较。主控 advisor-1.md 确认原前提错误；本轮只是下沉成页级函数，并分别诊断两项。
2. “低位计数待删”不适用于冻结代码：D07 已删递增；本轮删除残留高位及存储全链，见冻结 zPage.hpp:1052 和逐符号对照。
3. 删除 retained 副本后直接读已晋升 RegionInfo 的普通 map 不够：PromoteYoungRegion 替换 map。按 advisor-2.md 改为原 young 页拥有原 map，直至 relocation set 重置。未把中间状态当交付。

## 后续项

自动审批拒绝的两处直接 IsValidObject 过滤删除已恢复；对应/授权另经 sym_deliver new_issues 登记（Triage，① correctness）。本报告不把这两处判为等价或对齐完成。
