PROGRESS=DONE · verdict=current-main 后 55 项全绿，六个真实承重点 cut 36 发精确转红且恢复 10/10，OFF 符号差仅 CSWTCH 重编号 ｜尺=r4 direct/publication/managed+cut 矩阵 green=55 cut_red=36 restore_green=10 N=101 · LANE=impl_gcunit3_r3
DELIVERY_REF=cangjie_runtime|lane/gcunit3|025c4ff71e3e25ea266dc5b7af9765701c14a7d2
SIDE_EFFECT: 仅在本棒 kkk2 隔离树生成 ON/OFF 与瞬态 cut 产物；源码均按哈希恢复；未动 main、未 push
ROLE=implement
EVIDENCE=local:/root/cj_build/reports/REPORT-impl_gcunit3_r3.md,local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green,local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-product-arms,local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-managed-product-arms,local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-default-symbols

## 结论

返工完成。最终证据全部重取自已合入 `7612ba3b508bdb73648ae5056ad3aadb386a4382` 的 current-main 内容；旧 r3 产物不用于终态判词。

- T11 走 advisor 裁决的 **ⓑ 契约撤销**：三个 `Classifies*` 已改成 `RootFixFailsClosed*`，保留精确 `SIGABRT` 与共享 root 未写入断言，不向产品恢复 `Exit::RootFix` 诊断发射。
- residual-watermark 构造臂真实记录 `watermark_done=0`；四个普通臂同尺记录 `watermark_done=1`。
- native 与 managed 真实入口分别得到 `root_sites=0xb5/0xa5/0x84` 与 `root_sites=0xaa`；managed full/young 均只含 managed context 两位，不含 native context 两位。
- 任务书点名的四个产品承重点均以真实产品行切断；另补 managed mutator/watermark 两个真实分支切断。固定测试 ELF 下各红臂只红对应机制，恢复臂全绿。
- 默认 OFF:OFF current-main 与交付符号集合均为 5474 项；实测集合差不是空，而是 `CSWTCH.1502` 改号为 `CSWTCH.1501`。测试 hook 在 ON 命中、OFF 零命中。

CLAIM: current-main 内容同步后的本棒 8 项、forwarding publication 45 项、managed full/young 2 项全部成功且程序真实运行。
  METHOD: measure
  EVIDENCE: kkk2:/root/impl_gcunit3-aa48-run/evidence/impl_gcunit3/r4-current-green/results.tsv;kkk2:/root/impl_gcunit3-aa48-run/evidence/impl_gcunit3/r4-current-green/forwarding-publication.log;kkk2:/root/impl_gcunit3-aa48-run/evidence/impl_gcunit3/r4-current-green/managed-gate.log
  N: 55

CLAIM: current-main 十一项关键源码在 local 与 kkk2 的 sha256 逐项相同，合并带入的 routewit 六文件没有漏同步。
  METHOD: measure
  EVIDENCE: local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/preflight.log;可复跑命令=`sha256sum runtime/src/Heap/Allocator/ForwardingTable.{cpp,h} runtime/src/Heap/Collector/Collector.h runtime/src/Heap/WCollector/WCollector.h runtime/tests/gc_unit/{clear_entries_product_unit.cpp,test_forwarding_no_geometry.cpp,test_m0_exit.cpp,test_segmented_array_init.cpp} runtime/src/Heap/Collector/Mark.cpp runtime/src/ObjectModel/MArray.{cpp,h}`
  N: 11

## T11 裁决与契约

选择 ⓑ，理由是 advisor 已有现行裁决 `pending-gcunit-m0-failclosed-adjudication`：ZGC `zRelocate.cpp:382-416` 的调用层断言 receipt，没有 S0/S1 等价诊断分支；在没有反证 file:line 时维持“改测试不改产品”。

`ExpectRootFixControlledAbort` 在 `runtime/tests/gc_unit/test_m0_exit.cpp:383-400` 逐项断言 `WIFSIGNALED`、`WTERMSIG == SIGABRT`，并从共享映射读取 root 验证原 from 地址未被替换。三项名分别落在 `test_m0_exit.cpp:663-672,696-711`，名字只承诺 fail-closed 终止与 witness 形态，不再承诺已撤销的 S0/S1 分类。

CLAIM: T11 精确区分 SIGABRT，并跨 fork 验证共享 root 保持原值。
  METHOD: test
  EVIDENCE: M0Exit.RootFixFailsClosedOnUnmappedForwardedRoot/M0Exit.RootFixFailsClosedOnUnusableActiveWitness/M0Exit.RootFixFailsClosedOnUnusableRetiredWitness；故意切断真实 `FixMinorEvacuatedSlot(root, stw)` 后三项 rc=1，见 local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-product-arms/cut-fix-minor-slot/results.tsv；恢复三项 rc=0，见 local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-product-arms/restored/results.tsv

CLAIM: 产品对 `Exit::RootFix` 的 Note 调用为 0；同一检索尺对 ReadBarrier 有两个阳性命中。
  METHOD: read
  EVIDENCE: `git grep -n 'M0ExitDiagnostics::Note' -- runtime/src` => `runtime/src/Heap/Barrier/Barrier.cpp:1138,1142`，两行均为 `Exit::ReadBarrier`；`git grep -n 'M0ExitDiagnostics::Note(M0ExitDiagnostics::Exit::RootFix' -- runtime/src` => rc=1

久悬项 `pending-gcunit-nocopy-wiring-unproven` 随本轮改名一并了结：它的产品分类接线不可证，因为该分类契约已被 fail-closed 最终语义取代。

## residual-watermark 与 context 对照

普通 full/young 四项均记录 `phase_n=1 watermark_done=1`；构造项 `YoungGcResidualWatermarkUsesManagedFallback` 记录 `context=native-residual phase_n=1 watermark_done=0 root_sites=0xb5`，并在 `Mark.cpp:816-824` 走 false 分支到 `VisitMutatorRoots`。

CLAIM: residual-watermark 臂真实执行 false 分支；同尺四个普通臂提供 true 阳性对照。
  METHOD: control-arm
  EVIDENCE: local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/SegmentedArrayInit_YoungGcResidualWatermarkUsesManagedFallback.log;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/SegmentedArrayInit_EpochFlipRestartsAndRewritesPublishedBlock.log;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/SegmentedArrayInit_EpochFlipRestartsAndRewritesPublishedBlockParallel.log;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/SegmentedArrayInit_YoungGcRepairsIncompleteArrayRoot.log;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/SegmentedArrayInit_YoungGcRepairsIncompleteArrayRootParallel.log
  N: 5

managed full/young 语言入口由 `MArray.cpp:157-168` 在 `IsManagedContext()` 为真时请求产品 GC，并在 `MArray.cpp:190-205` 断言 managed mutator/watermark 位在场、native 两位不在场。两臂实测 `root_sites=0xaa`；native C++ 臂实测 `0x84/0xa5/0xb5`，构成双向 context 对照。

CLAIM: managed 与 native 真实入口分别命中各自 context 位，另一组位为 0；两组互为阳性对照。
  METHOD: control-arm
  EVIDENCE: local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/managed/segmented_array_managed.full.log;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/managed/segmented_array_managed.young.log;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/SegmentedArrayInit_EpochFlipRestartsAndRewritesPublishedBlock.log;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/SegmentedArrayInit_YoungGcRepairsIncompleteArrayRoot.log;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-current-green/SegmentedArrayInit_YoungGcResidualWatermarkUsesManagedFallback.log
  N: 5

## 产品接线证明

| 测试名/入口 | 它调的产品函数 (file:line) | 断开会红吗 | 证据 |
|---|---|---|---|
| 三个 `M0Exit.RootFixFailsClosed*` | `WCollector::FixMinorRootSlots` → `FixMinorEvacuatedSlot(root, stw)`，`runtime/src/Heap/Collector/Relocate.cpp:1241-1304` | 是，3/3 红；其余 segmented 5/5 绿 | `r4-product-arms/cut-fix-minor-slot/cut.diff` 与 `results.tsv` |
| native young/residual | `VisitMinorRootSlots` → `Mutator::VisitMutatorRoots` → native `VisitRawObjects`，`Mark.cpp:781-824`、`Mutator.cpp:392-413` | 是，3 个依赖项红；full 2 项与 T11 3 项绿 | `cut-mutator-root-walk/` |
| native completed-watermark | `GcPhaseEnum` → `DrainStackWatermark` → native `VisitRawObjects`，`Mutator.cpp:1011-1042,1183` | 是，4 个完成态项红；residual 与 T11 绿 | `cut-watermark-drain/` |
| incomplete-array full/range iterator | `BaseObject::ForEachRefField` 与 `MArray::ForEachRefFieldInRange` 的不可见对象 return，`BaseObject.cpp:105-116`、`MArray.cpp:215-225` | 是，segmented 5/5 红；T11 3/3 绿 | `cut-iterator-skip/` |
| managed full/young mutator walk | managed `VisitStackRoots` → `VisitRawObjects`，`Mutator.cpp:451-452` | 是，固定 ELF 下 full/young 均 rc=134 | `r4-managed-product-arms/fixed-elf/cut-mutator-managed-*.rc` |
| managed full/young watermark drain | managed `DrainStackWatermark` → `VisitRawObjects`，`Mutator.cpp:1057-1062` | 是，固定 ELF 下 full/young 均 rc=134 | `r4-managed-product-arms/fixed-elf/cut-watermark-managed-*.rc` |

四个任务书点名 cut 的 `cut.diff` 均直接删除上表产品承重行，不删除 `NoteLargeArrayInitRootVisit(...)`。managed 补充两刀也删除真实 `VisitRawObjects`，不是 receipt。

CLAIM: 四个点名产品 cut 在同一 8 项测试 ELF 下分别产生精确红集 `{3,4,3,5}`，恢复为 8/8 绿。
  METHOD: test
  EVIDENCE: 测试 ELF=`4819143a014cf0557a2b53686f2e52a4563949caddf248e7b8679571a0f3244b`；local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-product-arms/cut-mutator-root-walk/results.tsv;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-product-arms/cut-watermark-drain/results.tsv;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-product-arms/cut-fix-minor-slot/results.tsv;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-product-arms/cut-iterator-skip/results.tsv;恢复 local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-product-arms/restored/results.tsv

CLAIM: managed 两个真实承重点 cut 在同一个 ELF 下 full/young 四发均红，恢复两发均绿。
  METHOD: test
  EVIDENCE: 固定 ELF=`673c2c7fde40beb614ab3352fae9faab56015c3f314713e46b6749d89d6a9d95`；local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-managed-product-arms/fixed-elf/results.tsv；故意破坏 rc=134×4，恢复 rc=0×2

## 断线臂与同构型身份

点名四刀三臂的测试 ELF 恒为 `481914…`、boundscheck 恒为 `9e0de5…`；仅 runtime SO 改变。green 与 restored runtime SO 均为 `0fad8a…`，四个 cut SO 分别为 `f6bee6…`、`3d7cd4…`、`f20aab…`、`337d59…`。每个 `products.stat` 中 SO 生成时刻均晚于相应 `cut-source.stat`。

managed 固定 ELF 为 `673c2c…`；两个 cut SO 与 restored SO 分别记录在 `fixed-elf/*.products.sha256`。restored SO 回到 `0fad8a…`。四个原始产品文件恢复 `cmp=0`，managed 的 `Mutator.cpp` 恢复 `cmp=0`。

所有编译、运行命令均以 `taskset -c 16-31` 执行；两端 uptime 位于各臂 `preflight.log/postflight.log`。本轮是功能单测与构型身份，不外推性能结论。

## 承重面清单

直接消费者由以下机械检索得到：

```text
$ git grep -n -e 'NoteLargeArrayInitRootVisit' -e 'NoteLargeArrayInitRootPhase' -e 'ForceLargeArrayInitRootPhaseResidual' -- runtime/src
runtime/src/Common/BaseObject.cpp:112: NoteLargeArrayInitRootVisit(...ITERATOR_SKIP...)
runtime/src/Heap/Collector/Mark.cpp:715: NoteLargeArrayInitRootPhase(...MAJOR_MARK...)
runtime/src/Heap/Collector/Mark.cpp:786: NoteLargeArrayInitRootVisit(...MINOR_MARK...)
runtime/src/Heap/Collector/Mark.cpp:811,814: Force...MINOR_MARK / Note...MINOR_MARK
runtime/src/Heap/Collector/Relocate.cpp:1290,1415: NoteLargeArrayInitRootVisit(...MINOR_RELOCATE...)
runtime/src/Heap/Collector/Remembered.cpp:545: NoteLargeArrayInitRootVisit(...REMEMBERED...)
runtime/src/Mutator/Mutator.cpp:397: Note...MUTATOR_STACK_{MANAGED,NATIVE}
runtime/src/Mutator/Mutator.cpp:1019: Note...STACK_WATERMARK_{MANAGED,NATIVE}
runtime/src/ObjectModel/MArray.cpp:72,83,90: 三个 hook 定义
runtime/src/ObjectModel/MArray.cpp:219: Note...ITERATOR_SKIP
```

向上走到运行时入口的检索：

```text
$ git grep -n -e 'VisitStackRoots(' -e 'DrainStackWatermark(' -e 'VisitMinorRootSlots(' -e 'FixMinorRootSlots(' -- runtime/src/Heap runtime/src/Mutator
runtime/src/Heap/Collector/Mark.cpp:781,910: VisitMinorRootSlots 定义/入口
runtime/src/Heap/Collector/Relocate.cpp:1241,1683,1716: FixMinorRootSlots 定义/串并行入口
runtime/src/Mutator/Mutator.cpp:392,451: VisitStackRoots → StackManager
runtime/src/Mutator/Mutator.cpp:1011,1183: DrainStackWatermark 定义/调用
```

| 消费者/分支 | 产品 file:line | 有无臂 | 产物与说明 |
|---|---|---|---|
| mutator native | `Mutator.cpp:410-413` | 有 | `cut-mutator-root-walk` |
| mutator managed | `Mutator.cpp:451-452` | 有 | `cut-mutator-managed`，固定 ELF 双模式 |
| watermark native | `Mutator.cpp:1032-1042` | 有 | `cut-watermark-drain` |
| watermark managed | `Mutator.cpp:1057-1068` | 有 | `cut-watermark-managed`，固定 ELF 双模式 |
| minor residual false | `Mark.cpp:807-824` | 有 | `watermark_done=0` + mutator cut 红 |
| minor completed true | `Mark.cpp:807-818` | 有 | 四个 `watermark_done=1` + watermark cut 红 |
| relocation serial | `Relocate.cpp:1288-1304` | 有 | `cut-fix-minor-slot` |
| relocation parallel | `Relocate.cpp:1413-1428` | 无独立写回臂 | 与 serial 共用 `FixMinorEvacuatedSlot`；当前 incomplete old array 不需要写回，测试只把该 receipt 当路径观测，不外推为写回行为 |
| iterator full/range | `BaseObject.cpp:105-116`;`MArray.cpp:215-225` | 有 | 同一 patch 同时切两处 return，visitor 计数使 segmented 精确红 |
| remembered receipt | `Remembered.cpp:545` | 无 | 当前 `RequiredPhaseRootVisits` 不把 REMEMBERED 作为必达契约，故不以 receipt 外推产品行为 |
| managed 语言入口 | `MArray.cpp:157-205`;`gate_gc_unit.sh:348-356` | 有 | managed 两个真实 root consumer cut + 固定 ELF |

## 测试增删

坐标与集合差基于 `7612ba3b508bdb73648ae5056ad3aadb386a4382`。机械方法：分别对基线 `git show <sha>:<test.cpp>` 与工作树运行 `rg -o 'GC_(OTHER_VM_)?TEST\([^)]+' | sort`，再 `diff -u`。

- 新增：`SegmentedArrayInit.YoungGcResidualWatermarkUsesManagedFallback` —— 构造并断言 false 分支。
- 改名：
  - `RootFixRuntimeEnumerationClassifiesNoCopyAsS0` → `RootFixFailsClosedOnUnmappedForwardedRoot`
  - `RootFixClassifiesActiveOnlyUnusableCopyAsS1` → `RootFixFailsClosedOnUnusableActiveWitness`
  - `RootFixClassifiesRetiredOnlyUnusableCopyAsS1` → `RootFixFailsClosedOnUnusableRetiredWitness`
- 删除：无。三个旧名是依 advisor 正式撤销分类契约后的改名；精确 `SIGABRT` 与共享 root 安全断言未删。

## 双构型证明

| 构型 | CMake 变量 | 构建 rc | 状态 | 产品 SO sha256 |
|---|---|---:|---|---|
| 测试 ON | `Release; MRT_GC_UNIT_TESTS=ON; MRT_TESTABLE_INTERNALS=ON` | 0 | 8 项 + publication 45 项 + managed 2 项 PASS | `0fad8a8b3c2d1c233668e242bcce2bbb3619fece3a01071aba89a59c0e404150` |
| 产品 OFF | `Release; MRT_GC_UNIT_TESTS=OFF; MRT_TESTABLE_INTERNALS=OFF` | 0 | 构建 PASS；hook NOT_RUN | `441170c5dff5bc3cd391abfe98c217928e21681178c5289331a56bda046b1686` |

两臂都走 `cmake --build <build> --target cangjie-runtime --parallel 16`，均设置 `GC_UNIT_GATE_SKIP=1`；完整配方与 cache 值在 `r4-default-symbols/preflight.log`。宏只决定测试 hook/导出是否生成；默认路径的 `Mark.cpp:808-818` 条件提取保持一次求值和同一短路语义，不以测试宏门控正确性装置。

## 默认符号面

同配方 current-main 与 delivery 各 5474 项；实测 diff：

```diff
-CSWTCH.1502
+CSWTCH.1501
```

`self-noise.diff.rc=0`。同一 `nm --defined-only` 尺对 ON SO 的 `CJ_MRT_SetLargeArrayInitTestHooks` 命中 rc=0，对 OFF SO 零命中 rc=1。

CLAIM: 默认 OFF:OFF 的 defined-only 集合差非空且仅为一个编译器局部 CSWTCH 名重编号；测试 hook 没进入默认符号面。
  METHOD: control-arm
  EVIDENCE: local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-default-symbols/main-delivery.diff;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-default-symbols/hook-on.rc;local:/root/cj_build/cangjie_runtime_wt/y_gcunit3/evidence/impl_gcunit3/r4-default-symbols/hook-off.rc
  N: 2

## Harness 记账

`r4-current-green/standalone-build-and-probe.rc=1` 不作为绿证据：同一个 filter 在主 ELF 命中并通过，但在 forwarding publication ELF 零匹配，汇总器据此返回 1。该次调用只用于生成两个 ELF；随后本棒 8 项逐项直跑，publication ELF 无 filter 全跑 45/45，managed full/young 独立跑 2/2。终态判词只使用后三组直接结果。

## FALSIFIED

- 任务书旧前提“应恢复 S0/S1 分类”不成立：产品 `Exit::RootFix` 调用点为 0，且旧 NoCopy fixture 主动置 `FORWARDED`，在 `M0ExitDiagnostics.cpp:151-160` 会被计作 copyPublished/S1。advisor 据此维持 fail-closed 最终语义，正式撤销旧分类契约。
- 旧报告“默认构型无新增符号”不成立：最终同配方实测为 `CSWTCH.1502 → CSWTCH.1501`，已改成实际集合差。
- 远端旧 r3 树没有主控后来合入的 current-main 六文件；旧产物全部退出终态证据，r4 通过十一文件 sha256 同步后重建。
- 试图用 iterator 产品 cut 作为 managed 接线阳性对照时，managed full/young 都仍 rc=0；这证明 managed 的 iterator 位只是 receipt，不能证明 return 承重。该臂未用于放行，改以 managed mutator/watermark 两个真实 `VisitRawObjects` cut，固定 ELF 下四发 rc=134。
- OFF 构型之后直接增量切回 ON 首次链接 rc=2，原因是两个 CMake 树共享 `runtime/output/temp` 导致 OFF/ON 静态库交叉。源码 trap 已 `cmp=0`；对 ON 构型 scoped clean 后全量重建，SO 回到已知 `0fad8a…`，再重取 managed 红/绿臂。失败构建不用于行为结论。
