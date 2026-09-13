PROGRESS=DONE · verdict=外部阻塞：审批服务403，PR创建与报告指定路径归档未完成；尺=源码图/两构型编译，非行为验收 · LANE=sym_cangjie_runtime_465_implement_r5651041991
DELIVERY_REF=cangjie-runtime|sym/465-implement-r5651041991|1361f2896c47e55ea9b96e76313e7afcc3612972
SIDE_EFFECT: 常驻 1 Hz 统计线程与 per-CPU 存储；删除旧统计编译/环境开关；未改主分支。
ROLE=implement
EVIDENCE=local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence,kkk2:/root/sym_cangjie_runtime_465_implement_r5651041991

## ZGC 函数对应表

坐标：基线 `41b05e57b13c02aff9792588fcb97a4ba5161736`；候选 `1361f2896c47e55ea9b96e76313e7afcc3612972`。下表的 runtime 路径均相对本棒工作树 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991`；ZGC 为绝对参考树。命名组织按任务书自定，未新增 MRT_GCV2 开关。此表为实现读证，待独立 Review。

|机制/函数|我方 file:line|ZGC file:line|
|---|---|---|
|指标 identity / per-CPU offset|`runtime/src/Base/ZStat.cpp:168`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:342`|
|常驻对齐存储|`runtime/src/Base/ZStat.cpp:175`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zUtils.inline.hpp:37`|
|CPU 数与 CPU id|`runtime/src/Base/ZStat.cpp:186`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zCPU.inline.hpp:32`|
|sampler 构造登记|`runtime/src/Base/ZStat.cpp:209`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:432`|
|counter 构造登记|`runtime/src/Base/ZStat.cpp:243`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:468`|
|sampler CPU 数据初始化|`runtime/src/Base/ZStat.cpp:215`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:362`|
|counter CPU 数据初始化|`runtime/src/Base/ZStat.cpp:249`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:362`|
|原子采样：count/sum/max|`runtime/src/Base/ZStat.cpp:220`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:892`|
|collect-and-reset|`runtime/src/Base/ZStat.cpp:229`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:440`|
|counter increment|`runtime/src/Base/ZStat.cpp:254`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:922`|
|counter tick sample-and-reset|`runtime/src/Base/ZStat.cpp:259`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:476`|
|registry 初始化|`runtime/src/Base/ZStat.cpp:268`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:362`|
|聚合数据 Add/Average|`runtime/src/Base/ZStat.h:82`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:61`|
|窗口 Add/Total/Accumulated|`runtime/src/Base/ZStat.h:96`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:89`|
|三级 history Add/Windows|`runtime/src/Base/ZStat.h:127`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:156`|
|phase 构造登记/结束采样|`runtime/src/Base/ZStat.h:225`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:597`|
|Timer 构造与结束生产|`runtime/src/Base/LogFile.h:280`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.hpp:301`|
|tick 按 id 汇入 history|`runtime/src/Base/ZStat.cpp:282`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:1029`|
|统计线程启动|`runtime/src/Base/ZStat.cpp:324`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:1022`|
|1 Hz cadence / history 生命期|`runtime/src/Base/ZStat.cpp:344`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:1070`|
|线程终止 / join|`runtime/src/Base/ZStat.cpp:332`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:1093`|
|排序/窗口均值与最大值输出|`runtime/src/Base/ZStat.cpp:292`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:1055`|
|分配率 initialize|`runtime/src/Base/ZStat.cpp:448`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:945`|
|分配率采样粒度|`runtime/src/Base/ZStat.cpp:431`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:951`|
|分配生产/速率采样|`runtime/src/Base/ZStat.cpp:475`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:957`|
|分配率 stats 消费|`runtime/src/Base/ZStat.cpp:511`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:1014`|
|已有按代 cycle 数列|`runtime/src/Base/ZStat.cpp:51`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:1242`|
|generation 时长身份|`runtime/src/Heap/Collector/CollectorResources.cpp:367`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:711`|
|collection 时长身份（含 major prelude）|`runtime/src/Heap/Collector/CollectorResources.cpp:405`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:667`|
|按代 reclaimed 数列/常驻 sampler|`runtime/src/Base/ZStat.cpp:540`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:1818`|
|heap stats 同步快照|`runtime/src/Base/ZStat.cpp:551`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:1901`|
|director 单快照消费|`runtime/src/Base/ZStat.cpp:96`|`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:1307`|

⚠ **明确未对齐项：oldLive 的数值来源**。`runtime/src/Heap/Collector/CopyCollector.cpp:144` 依 advisor 保留基线 oldLive=周期末 used 标量；ZGC `zStat.cpp:1788-1800` 从 selector 各组 live 累加。我方 mark-end 真实 live 汇总待 A07（livemap 惰性初始化，#464 在飞）合入后接入。不得把本交付表述为该数值语义已对齐。裁定：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_465_implement_r5651041991-20260913T042850Z.md:2`。Young 使用既有标记结果 `gcStats.youngPromotedBytes`；不从 candidate-reclaimed 推算。

平台适配：ZStat 用现有 C++ 线程、条件变量和 LOG 输出承担 ZThread/ZMetronome/LogTarget 的工作；Linux 用实际 CPU id，CPU-id 不可用时按 JDK 平台回退取 CPU 0。输出以 ns/B 等原生单位标注。既有 GCLOG 所需的 STW 深度观察保留在 `ZStat::EnterStwScope/ExitStwScope/WorldStoppedNow`，它不登记身份、不选择 sampler、不参与 history 生命周期；Timer 的 rec=phase/leaf 与 STW 的 rec=stw 出口继续承担原观察合同。

CLAIM: sampler/counter 的 identity 与链表成员关系由构造登记，per-CPU 槽在初始化后常驻
  METHOD: read
  EVIDENCE: runtime/src/Base/ZStat.cpp:168; runtime/src/Base/ZStat.cpp:209; runtime/src/Base/ZStat.cpp:243; runtime/src/Base/ZStat.cpp:268

CLAIM: 统计线程以 1 Hz 消费 counter/sampler，按 id 汇入 10s/10m/10h/总计 history，再按 log level 输出
  METHOD: read
  EVIDENCE: runtime/src/Base/ZStat.cpp:344; runtime/src/Base/ZStat.cpp:282; runtime/src/Base/ZStat.h:127; runtime/src/Base/ZStat.cpp:292

## 静态登记 → 生产器 → tick → history → 输出

变更前顺序表：`local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/producer-consumer.md`。机械检索全文：`local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/producer-chain.txt`。所有下表 phase 生产器都经过 `Base/LogFile.h` 的 Timer → `ZStatPhase::RegisterEnd` → `ZStatSampler::Sample`，之后同走 `ZStat.cpp:282` tick → `ZStat.h:129` history → `ZStat.cpp:292` Print。ZGC 对应是 `zStat.hpp:301` Timer → `zStat.cpp:826` SubPhase register_end / `zStat.cpp:918` DurationSample → `zStat.cpp:1029` tick → `zStat.cpp:174` history → `zStat.cpp:1055` print。

|静态身份（构造锚）|group / 所属域|实际生产调用点|
|---|---|---|
|`PCollectFromSpaceGarbage` / `CollectFromSpaceGarbage` (`Base/ZStat.cpp:366`)|Old Subphase|`runtime/src/Heap/Collector/RelocationSet.cpp:100`|
|`PCollectLargeGarbage` / `Collect large garbage` (`Base/ZStat.cpp:367`)|Old Subphase|`runtime/src/Heap/WCollector/WCollector.h:1033`|
|`PConcurrentMarking` / `Concurrent marking` (`Base/ZStat.cpp:368`)|Old Subphase|`runtime/src/Heap/Collector/TracingCollector.cpp:818`|
|`PConcurrentReMarking` / `Concurrent re-marking` (`Base/ZStat.cpp:369`)|Old Subphase|`runtime/src/Heap/Collector/TracingCollector.cpp:823`|
|`PConcurrentResurrection` / `concurrent resurrection` (`Base/ZStat.cpp:370`)|Old Subphase|`runtime/src/Heap/Collector/TracingCollector.cpp:850`|
|`PDoTracing` / `DoTracing` (`Base/ZStat.cpp:371`)|Old Subphase|`runtime/src/Heap/Collector/TracingCollector.cpp:814`|
|`PEnumRootsUpdateOldPointersWithin` / `enum roots & update old pointers within` (`Base/ZStat.cpp:372`)|Old Subphase|`runtime/src/Heap/Collector/Mark.cpp:636`|
|`PExemptFromRegions` / `ExemptFromRegions` (`Base/ZStat.cpp:373`)|Old Subphase|`runtime/src/Heap/Allocator/RegionSpace.h:163`|
|`PFinalizer` / `Finalizer` (`Base/ZStat.cpp:374`)|Critical|`runtime/src/Heap/Collector/FinalizerProcessor.cpp:110`; `runtime/src/Heap/Collector/FinalizerProcessor.cpp:359`|
|`PFinalizerProcessorWaittingTime` / `finalizerProcessor waitting time` (`Base/ZStat.cpp:375`)|Critical|`runtime/src/Heap/Collector/FinalizerProcessor.cpp:110`|
|`YoungForwardFromRegions` / `ForwardFromRegions` (`Base/ZStat.cpp:376`)|Young Subphase|`runtime/src/Heap/Allocator/RegionSpace.h:180`|
|`OldForwardFromRegions` / `ForwardFromRegions` (`Base/ZStat.cpp:377`)|Old Subphase|`runtime/src/Heap/Allocator/RegionSpace.h:181`|
|`PIdentifyUselessExternRef` / `identify useless extern ref` (`Base/ZStat.cpp:378`)|Old Subphase|`runtime/src/Heap/Collector/TracingCollector.cpp:844`|
|`POldRelocateStart` / `old.relocate_start` (`Base/ZStat.cpp:379`)|Old Pause|`runtime/src/Heap/Collector/Relocate.cpp:590`|
|`PPostTrace` / `PostTrace` (`Base/ZStat.cpp:380`)|Old Subphase|`runtime/src/Heap/Collector/RelocationSet.cpp:73`|
|`PPreforward` / `Preforward` (`Base/ZStat.cpp:381`)|Old Subphase|`runtime/src/Heap/Collector/Relocate.cpp:581`|
|`PReclaimGarbageRegions` / `ReclaimGarbageRegions` (`Base/ZStat.cpp:382`)|Critical|`runtime/src/Heap/Allocator/RegionSpace.h:107`|
|`PReleaseGarbageMemory` / `ReleaseGarbageMemory` (`Base/ZStat.cpp:383`)|Critical|`runtime/src/Heap/Allocator/RegionSpace.h:111`|
|`PRemapYoungRoots` / `RemapYoungRoots` (`Base/ZStat.cpp:384`)|Old Subphase|`runtime/src/Heap/Collector/Relocate.cpp:384`|
|`PTraceLiveObjectsUpdateOldPointersInRefFields` / `trace live objects & update old pointers in ref-fields` (`Base/ZStat.cpp:385`)|Old Subphase|`runtime/src/Heap/Collector/Mark.cpp:679`|
|`PTryReclaimGarbageRegions` / `TryReclaimGarbageRegions` (`Base/ZStat.cpp:386`)|Critical|`runtime/src/Heap/Allocator/RegionSpace.h:138`|
|`PTryReleaseGarbageMemory` / `TryReleaseGarbageMemory` (`Base/ZStat.cpp:387`)|Critical|`runtime/src/Heap/Allocator/RegionSpace.h:141`|
|`PYoungConcPromoteWalk` / `young.conc_promote_walk` (`Base/ZStat.cpp:388`)|Young Subphase|`runtime/src/Heap/Collector/Relocate.cpp:1756`|
|`PYoungConcurrentRelocate` / `young.concurrent_relocate` (`Base/ZStat.cpp:389`)|Young Subphase|`runtime/src/Heap/Collector/Relocate.cpp:1632`|
|`PYoungEvacFinish` / `young.evac_finish` (`Base/ZStat.cpp:390`)|Young Subphase|`runtime/src/Heap/Collector/Relocate.cpp:1689`|
|`PYoungEvacRetire` / `young.evac_retire` (`Base/ZStat.cpp:391`)|Young Subphase|`runtime/src/Heap/Collector/Relocate.cpp:1775`|
|`PYoungFlushAlloc` / `young.flush_alloc` (`Base/ZStat.cpp:392`)|Young Subphase|`runtime/src/Heap/Collector/Generation.cpp:765`|
|`PYoungMarkClosure` / `young.mark_closure` (`Base/ZStat.cpp:393`)|Young Subphase|`runtime/src/Heap/Collector/Generation.cpp:1020`|
|`PYoungMarkFollow` / `young.mark_follow` (`Base/ZStat.cpp:394`)|Young Subphase|`runtime/src/Heap/Collector/Mark.cpp:1472`|
|`PYoungMarkFromRemset` / `young.mark_from_remset` (`Base/ZStat.cpp:395`)|Young Subphase|`runtime/src/Heap/Collector/Generation.cpp:1068`|
|`PYoungPinnedScan` / `young.pinned_scan` (`Base/ZStat.cpp:396`)|Young Subphase|`runtime/src/Heap/Allocator/RegionManager.cpp:269`|
|`PYoungPostEvacFinish` / `young.post_evac_finish` (`Base/ZStat.cpp:397`)|Young Subphase|`runtime/src/Heap/Collector/Generation.cpp:1314`|
|`PYoungPreEvacClear` / `young.pre_evac_clear` (`Base/ZStat.cpp:398`)|Young Subphase|`runtime/src/Heap/Collector/Generation.cpp:1239`|
|`PYoungPrepareCandidates` / `young.prepare_candidates` (`Base/ZStat.cpp:399`)|Young Subphase|`runtime/src/Heap/Collector/Generation.cpp:788`|
|`PYoungRefFix` / `young.ref_fix` (`Base/ZStat.cpp:400`)|Young Subphase|`runtime/src/Heap/Collector/Relocate.cpp:1554`; `runtime/src/Heap/Collector/Relocate.cpp:1580`; `runtime/src/Heap/Collector/Relocate.cpp:1615`; `runtime/src/Heap/Collector/Relocate.cpp:1646`|
|`PYoungRefFixBulk` / `young.ref_fix_bulk` (`Base/ZStat.cpp:401`)|Young Subphase|`runtime/src/Heap/Collector/Relocate.cpp:1646`|
|`PYoungRefFixPrepare` / `young.ref_fix_prepare` (`Base/ZStat.cpp:402`)|Young Subphase|`runtime/src/Heap/Collector/Relocate.cpp:1580`|
|`PYoungRefFixRootPass1` / `young.ref_fix_root_pass1` (`Base/ZStat.cpp:403`)|Young Subphase|`runtime/src/Heap/Collector/Relocate.cpp:1615`|
|`PYoungRemsetDrain` / `young.remset_drain` (`Base/ZStat.cpp:404`)|Young Subphase|`runtime/src/Heap/Collector/Generation.cpp:855`|
|`PYoungRemsetRescan` / `young.remset_rescan` (`Base/ZStat.cpp:405`)|Young Subphase|`runtime/src/Heap/Collector/Generation.cpp:1058`|
|`PYoungRootEnum` / `young.root_enum` (`Base/ZStat.cpp:406`)|Young Subphase|`runtime/src/Heap/Collector/Generation.cpp:955`|
|`YoungGeneration` / `Young Generation` (`Base/ZStat.cpp:407`)|Young Generation|`runtime/src/Heap/Collector/CollectorResources.cpp:385`|
|`OldGeneration` / `Old Generation` (`Base/ZStat.cpp:408`)|Old Generation|`runtime/src/Heap/Collector/CollectorResources.cpp:385`|
|`MinorCollection` / `Minor Collection` (`Base/ZStat.cpp:409`)|Minor Collection|`runtime/src/Heap/Collector/CollectorResources.cpp:444`|
|`MajorCollection` / `Major Collection` (`Base/ZStat.cpp:410`)|Major Collection|`runtime/src/Heap/Collector/CollectorResources.cpp:444`|

其它两条真实链：
- allocation：`runtime/src/Heap/Allocator/RegionManager.cpp:1881` → `ZStat.cpp` 的 allocation counter 与原 allocation-rate 窗口 → tick/history/Print；director 从 `ZStatMutatorAllocRate::stats` 读取预测快照。
- heap/cycle：`runtime/src/Heap/Collector/CopyCollector.cpp:145` → 所属 generation 的 ZStatHeap 数列及 Reclaimed sampler；`CollectorResources::RunCollection` 与 `ExecuteDriverRequest` 分别记录 generation 与整个 collection，不按 phase 名称推代。

## 删除清单

|基线旧机制/路径|替代|
|---|---|
|`Base/ZStat.h` PhaseTotals/Table/RegisteredPhases、`Base/ZStat.cpp` TableLock/CycleTable/NotePhase/NoteCycleEnd、cycle 结束清表|静态 ZStatValue identity、per-CPU sampler/counter、统计线程拥有 history|
|`Base/ZStat.cpp` Enabled/env override/test override；`runtime/config.cmake` MRT_ZSTAT option 与 `runtime/CMakeLists.txt` MRT_ZSTAT_COMPILED 分支|常驻采样；仅既有 LOG level 控制输出|
|`Heap/Collector/MutatorAllocRate.h` 与 `.cpp`，Collector/CMakeLists 中该源登记|本体迁入 `Base/ZStat.h/.cpp`，allocator/director 直接调用 ZStatMutatorAllocRate；无转发兼容类|
|`GcStats.cpp` g_youngDurationSeq/g_oldDurationSeq、g_youngReclaimedSeq/g_oldReclaimedSeq 及其发布 atomics|已有 generation ZStatCycle 数列；ZStatHeap 按代 reclaimed 数列和 sampler|
|`GcStats.h` lastYoung/lastOld/lastGcDuration、warmup/isWarm/isTimeTrustable 重复字段及 CopyCollector 写入|已有常开 ZStatCycle 与其 director 快照|
|测试目标中额外编译的 `Base/ZStat.cpp`|两种测试构建入口都链接产品 SO 中的单一实现|

未删除 GCStats 的既有日志、HEU 行为字段：它们不属于被 ZStat 覆盖的统计本体，本轮不改其它包机制。

基线逐符号锚（完整原文同附录）：

|旧符号|基线 file:line|
|---|---|
|`RegisteredPhases`|`runtime/src/Base/ZStat.cpp:276`|
|`CycleTable`|`runtime/src/Base/ZStat.cpp:166`|
|`TableLock`|`runtime/src/Base/ZStat.cpp:160`|
|`PhaseTotals`|`runtime/src/Base/ZStat.cpp:211`|
|`NotePhase`|`runtime/src/Base/LogFile.h:341`|
|`NoteCycleEnd`|`runtime/src/Base/ZStat.cpp:229`|
|`SetEnabledForTest`|`runtime/src/Base/ZStat.cpp:306`|
|`g_enabledOverride`|`runtime/src/Base/ZStat.cpp:158`|
|`MRT_ZSTAT_COMPILED`|`runtime/src/Base/ZStat.cpp:148`|
|`MRT_ZSTAT`|`runtime/src/Base/ZStat.cpp:179`|
|`g_youngDurationSeq`|`runtime/src/Heap/Collector/GcStats.cpp:30`|
|`g_oldDurationSeq`|`runtime/src/Heap/Collector/GcStats.cpp:31`|
|`g_youngReclaimedSeq`|`runtime/src/Heap/Collector/GcStats.cpp:32`|
|`g_oldReclaimedSeq`|`runtime/src/Heap/Collector/GcStats.cpp:33`|
|`MutatorAllocRate`|`runtime/src/Base/ZStat.cpp:17`|
|`g_sampleCount`|`runtime/src/Heap/Collector/MutatorAllocRate.cpp:41`|
|`GCStats::lastOldDurationNs`|`runtime/src/Heap/Collector/GcStats.cpp:20`|
|`GCStats::reclaimedPerYoungAvg`|`runtime/src/Base/ZStat.cpp:142`|
|`GCStats::usedAtLastMajorEnd`|`runtime/src/Base/ZStat.cpp:140`|

旧文件本体：`runtime/src/Heap/Collector/MutatorAllocRate.cpp:48`、`runtime/src/Heap/Collector/MutatorAllocRate.h:24`。

候选 `git grep` 零命中原文如下（rc=1 表示匹配不到，不是命令未运行）。基线同命令阳性原文见 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/deletions.txt`；其中包括旧构造/调用/分支，不以程序未启动造成的空输出来证明删除。

```text
$ git grep -n -w RegisteredPhases -- runtime/src
rc=1
$ git grep -n -w CycleTable -- runtime/src
rc=1
$ git grep -n -w TableLock -- runtime/src
rc=1
$ git grep -n -w PhaseTotals -- runtime/src
rc=1
$ git grep -n -w NotePhase -- runtime/src
rc=1
$ git grep -n -w NoteCycleEnd -- runtime/src
rc=1
$ git grep -n -w SetEnabledForTest -- runtime/src
rc=1
$ git grep -n -w g_enabledOverride -- runtime/src
rc=1
$ git grep -n -w MRT_ZSTAT_COMPILED -- runtime/src
rc=1
$ git grep -n -w MRT_ZSTAT -- runtime/src
rc=1
$ git grep -n -w g_youngDurationSeq -- runtime/src
rc=1
$ git grep -n -w g_oldDurationSeq -- runtime/src
rc=1
$ git grep -n -w g_youngReclaimedSeq -- runtime/src
rc=1
$ git grep -n -w g_oldReclaimedSeq -- runtime/src
rc=1
$ git grep -n -w MutatorAllocRate -- runtime/src
rc=1
$ git grep -n -w g_sampleCount -- runtime/src
rc=1
$ git grep -n -w GCStats::lastOldDurationNs -- runtime/src
rc=1
$ git grep -n -w GCStats::reclaimedPerYoungAvg -- runtime/src
rc=1
$ git grep -n -w GCStats::usedAtLastMajorEnd -- runtime/src
rc=1
```

CLAIM: 删除清单中的旧 identity/table、覆盖的统计序列与旧开关符号在候选 runtime/src 精确检索无匹配
  METHOD: measure
  EVIDENCE: local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/deletions.txt（逐命令 stdout 与 rc，候选臂）
  N: 19 个精确符号检索

CLAIM: 同一检索器在冻结基线匹配到上述旧机制定义或调用，构成删除检索的阳性对照
  METHOD: measure
  EVIDENCE: local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/deletions.txt（基线臂，各查询原文）
  N: 19 个精确符号检索

## 双构型构建

配方原文 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/build.sh`。两构型同时启动，独立 build/install 目录，各 `-j$(nproc)`，实际 jobs=192，并行臂数=2；CCACHE_DIR=/root/.ccache，C/C++/ASM launcher=ccache，prefix-map 配方原文在脚本。所有步骤 wall 均见对应 rc 文件；未发生 wall>10min 的顺序步骤。

两构型为 `MRT_GC_UNIT_TESTS=OFF` 与 `ON`（后者开启既有 MRT_TESTABLE_INTERNALS），Release、COPYGC_FLAG=1。旧 MRT_ZSTAT 开关已删除，两构型都包含统计产品代码。`GC_UNIT_GATE_SKIP=1`；未执行 unit、切刀、entry_cut_check 或 nwdet，构建后既有 gate 日志明确 `GC_UNIT_GATE_NOT_RUN reason=EXPLICIT_SKIP`。内置 configure 探测只作为构建日志保留，不扩张为统计行为证据。

|构型|cmake 变量|configure rc/wall|build rc/wall|runtime SO sha256|
|---|---|---|---|---|
|default|`MRT_GC_UNIT_TESTS=OFF`|`CONFIGURE_RC=0 wall=53`|`BUILD_RC=0 wall=17`|`d700d58ee824a03893b058aaf724525d10c2867fc50031027717c83b217b7d27`|
|testable|`MRT_GC_UNIT_TESTS=ON`|`CONFIGURE_RC=0 wall=53`|`BUILD_RC=0 wall=17`|`ae53ec4698e51f64596dd4bb74f254f476b1b3e5764663e54dd5e8ae37b0413a`|

两构型 boundscheck SHA256：`31cec902ab00f30d64aef5d04bdc54a2d91579689d94db4d5bf4108015025514`。完整 SO 路径与血缘在 `artifacts.json`，两枚 SO 各自已存入 `/root/sodepot/1361f2896c47e55ea9b96e76313e7afcc3612972/`（testable 子目录）。两构型 `CJRT-COMMIT` 均为候选 HEAD。

```text
 12:41:14 up 3 days, 12:49,  1 user,  load average: 0.53, 4.02, 5.76
 12:42:24 up 3 days, 12:50,  1 user,  load average: 13.76, 6.68, 6.52
```


构建过程原样保留：attempt0 两构型 configure rc=1（配方工作目录错，现有 CMake 相对 CJThread 入口找不到 schedule.h）；attempt1 configure rc=0/build rc=1（本轮引入 std::align_val_t，产品 C++14 不支持）；随后改为 ZGC 的 malloc+padding 地址对齐，中间态两构型 build rc=0。最终 rc 与身份以本节表为准。

CLAIM: 两构型编译结果由实际 configure/build 退出码与链接产物摘要记录
  METHOD: measure
  EVIDENCE: local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/default.rc; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/testable.rc; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/artifacts.json; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/provenance-default.txt; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/provenance-testable.txt
  N: 2 个构型，各 1 次最终构建

CLAIM: 编译器对本轮 C++14 不支持的对齐表达式返回非零，修正前错误诊断被保留，证明编译装置实际执行
  METHOD: measure
  EVIDENCE: local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/attempt1/build-default.log; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/attempt1/build-testable.log; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/attempt1/default.rc; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/attempt1/testable.rc
  N: 2 个构型

CLAIM: 常驻 sampler/counter、SampleAndCollect/Run/history 在两构型产品完整符号表中存在，不以关闭宏代替存在性证明
  METHOD: measure
  EVIDENCE: local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/nm-default.txt; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/nm-testable.txt; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/stat-symbols.txt; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/artifacts.json
  N: 2 枚 runtime SO

## 测试同批迁移 / 集合差

参考树检索范围 `test/hotspot/gtest/gc/z` 与 `test/hotspot/jtreg/gc/z` 中无 ZStat/ZStatistics 专用测试匹配，原文 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/reference-tests.txt`。阳性对照同文件先列出实际测试源（例如 test_zArray.cpp、test_zAddress.cpp，目录枚举 rc=0），再做内容查询 rc=1。因此不编造不存在的 ZGC 测试名；本地测试按上述 ZStat 函数不变量新增/替换。

|本地测试|ZGC 对应函数|变更|
|---|---|---|
|RegistryExistsBeforeSampling|ZStatIterableValue 构造/insert，zStat.cpp:386-403|替换 RegistryEnumeratesObservedPhases；静态身份不以观察触发|
|PauseAndConcurrentKeepStaticIdentity|ZStatPhase 构造与 ZStatSample，zStat.cpp:600/883|替换 PauseAndConcurrentAreSeparateAccounts、KindIsSampledAtScopeEntry、MaxPauseTracksLargestPauseSample；两个静态 group + 最大值断言|
|CounterTickConsumesAndRetainsHistory|ZStatCounter::sample_and_reset / ZStat::sample_and_collect|新增 counter→tick→history|
|HistoryRollsOverAllThreeLevels|ZStatSamplerHistory::add|新增三级滚动与总计/最大值保留|
|HistoryIncludesPartialIntervals|ZStatSamplerHistory avg/max getters|新增未满分层窗口合并|
|StwDepthCounterClassifies|既有 GCLOG 观察合同（不作为 ZGC registry 能力）|保留|
|ZeroDurationSampleStillCounts|ZStatSample/collect_and_reset|保留，迁移到 sampler API|

机械集合差 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/test-set-diff.txt`。本轮不为了判据变绿改测试或添加豁免；旧用例替换的原因是被测旧机制确已删除。

测试构建尝试：`cj_gc_unit` 在提交 8084195d2aaf372d5ff8b0e9b620a3ef8fe6776d 编译目标 rc=1、wall=47s；原始日志 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/remote/build-test-sources.log`、`test-sources.rc`、`test-sources.head`。失败涉及既有 fixture 的旧 forwarding API、ResolveStoreValue 签名和 worker 参数；失败文件与基线 blob 身份对照见 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/test-failure-content-identity.txt`。本包 test_zstat.cpp 已生成对象；未执行任何测试，不宣称测试通过。最终测试 TU 构建结果：`TEST_ZSTAT_OBJECT_RC=0 wall=43`，对象 SHA256 `9b99c98da02ff793daeecaf861ba55c464374f259a4aed91737e7e219dabcfcf`；见 `test-zstat-object.rc`。未生成测试 ELF，六栏卡的测试 ELF 摘要不适用。

## 产品接线证明

|对象|产品接线|行为验证状态|
|---|---|---|
|phase sampler|静态 phase → 产品 Timer → RegisterEnd → Sample|源码图完整；本轮不跑行为臂|
|allocation counter|RegionManager 成功分配 → ZStatMutatorAllocRate → counter → tick/history|源码图完整；本轮不跑行为臂|
|generation/collection|RunCollection / ExecuteDriverRequest → 相应静态 phase|源码图完整；本轮不跑行为臂|
|history/输出|统计线程 → SampleAndCollect → history Windows → Print|两构型符号在场；运行时行为未验证|

## 承重面清单

机械导出为 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/producer-chain.txt`，包含每个 phase 调用、allocator 出口、driver 两条身份出口、history 与线程生命周期。全部未执行切刀/三臂：依据本轮 alignment_mode 明确例外。不存在红臂行为验收结论。

## 授权与候选边界

- 首次 advisor 允许 GcStats/MutatorAllocRate 本体、Timer 及各统计调用点，`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_465_implement_r5651041991-20260913T040604Z.md:2`。
- 第二次允许仅删除两处父级构建旧开关登记，确认 Timer 实际在 LogFile.h，`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_465_implement_r5651041991-20260913T041609Z.md:2`。
- 第三次 oldLive 裁定见上文，保留语义并分派 A07 后续。
- 基线 rev-parse rc=0，完整身份及 commit payload sha256 / Git object SHA1 独立复算见 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_465_implement_r5651041991/evidence/identity.txt`。只推 cjcjdev 的本候选分支。

|候选提交|机制|
|---|---|
|`296e4ffd7`|feat(gc): port resident statistics registry and history (zStat.cpp:386,1036)|
|`08c97df1e`|refactor(gc): remove ZStat compile gate (zStat.cpp:1029)|
|`02f5ca18f`|fix(gc): align permanent statistics storage in C++14 (zUtils.inline.hpp:37)|
|`4b5ca400b`|fix(gc): retain scoped heap statistic semantics (zStat.cpp:1788)|
|`8084195d2`|test(gc): bind statistics tests to resident product registry (zStat.cpp:386)|
|`1361f2896`|fix(gc): bind forwarding phase identity to its generation (zStat.cpp:805)|

首笔同时迁移 registry/history 与统计生产消费本体，判词只对本包整体成立，不把提交数当独立变量数。

## FALSIFIED

1. 指定 zStat.inline.hpp 在参考树不存在；采样实现实际位于 zStat.cpp:883 起。已获 advisor 确认，按实际文件对齐。
2. 原排他文件集不足以迁移 GcStats/MutatorAllocRate 和真实 Timer；通过两次 advisor 获准精确扩展，未自行扩大其它机制。
3. 实现中初次选用 std::align_val_t 不符合产品 C++14，两个实际编译失败后按 zUtils.inline.hpp:37-49 修正，未改编译标准。
4. 初次运输包漏根 tree、首次构建配方工作目录错误，均已修正后再记录产品编译结果；不把这些失败当作产品行为证据。
5. oldLive 真实 mark-end 汇总不是现有基线语义，依明确裁定留作 A07 后续；移除中间态 candidate-reclaimed 推算。

## 后续项

SYM-NEXT: stage=Triage new-issue: cangjie-runtime | ZStatHeap old live 改为 mark-end livemap 汇总（前置 A07） | Triage | ③ perf
SYM-NEXT: stage=Triage new-issue: cangjie-runtime | gc_unit 测试目标仍引用冻结基线已移除的产品接口，无法完成构建 | Triage | infra

形式检查未执行：指定报告更新依赖已失败的审批服务；现有指定路径报告仍为早期 WIP，不能代替本份候选证据。
PR 创建被自动审批服务拒绝，gh pr create 未执行；已准备正文见同目录 pr-body.md。候选分支已成功推送至 cjcjdev，HEAD=1361f2896c47e55ea9b96e76313e7afcc3612972。

自动审批服务记录：补充清理完成接口的命令在执行前被服务拒绝，403 Forbidden（This account only allows Codex official clients）；工作树产品文件保持已推送候选不变。该命令没有产生未提交修改。

## 外部阻塞与恢复入口

自动审批服务连续拒绝补充提交与创建 PR 命令，原因原文：`403 Forbidden: This account only allows Codex official clients`。拒绝发生在进程创建前，不是编译或测试失败。产品工作树保持已推送 HEAD；未绕过审批，未动主分支。

PR 标题：feat(gc): A12a resident ZStat registry, samplers and history。
PR 正文：本目录 pr-body.md；完整源码表与证据位于本工作树 evidence/。
恢复后待执行：归档 evidence 到指定 EVIDENCE 目录、复制本报告到任务唯一报告路径、创建/复用候选 PR、填写 SYM-PR、运行 deliver check、重新登记 next_stage=Review。

补充删除 GcStats 四参数完成接口的命令未执行；原有 HEU 时钟接口保留，不包含已迁出的统计存储。

最后一次结构化登记也被同一自动审批服务 403 拒绝，sym_deliver 未成功。因此实际机器状态仍为前次 WIP；本地终态文稿不等同于已登记。指定报告写入也在进程创建前被拒绝，未覆盖原文件。恢复所需材料全部保留，产品工作树无未提交修改。
