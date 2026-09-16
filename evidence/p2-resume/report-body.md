## 状态与坐标

本轮保留原候选分支/PR #631。指定仓回读 cjcjdev/main rc=0，实际为2677e74382846076984b0adf4e5df5e953b46dad；派发冻结基线ec8acc48fffd6badb68c6736ee25907216359fc5。已按任务合同合入main，冲突处保留P01公共颜色及非法heap输入检查。产品源码构建坐标0819a5dda2dcac5712e161e060a4fdaa459d1b6d，随后提交仅补测试/证据；产品子树身份见final-git.json。

本报告保持WIP，**不送Review、不宣告整包验收**。主控172829Z要求最终消费cjcj#48独审合入的compiler+受影响stdlib+ELF，再跑原spawn和finalizer_trigger。当前#48仍在实现且依赖runtime#646；P2_PLAIN_MAIN只隔离本条字段测试，不替代原始真实输入验收。其余未闭合面见文末。没有改main或共享SDK。

## 授权与ZGC对应

协调面全部为/root/cj_build/ops。新增mark期Finalizable生产接缝获advisor 20260915T172100Z明确授权，规格为/root/cj_build/reports/REPORT-release002_finalizable_phase_0916.md §3–5。不是本棒擅自扩大到P13整体迁移。

| 分路/数据 | 本实现（runtime/src/） | ZGC（/root/cj_build/reference/jdk/src/hotspot/share/gc/z/） |
|---|---|---|
| 字段上下文选Strong/Finalizable fast、slow、color | Heap/z/zBarrier.inline.hpp:125，Heap/z/zMark.cpp:195 | zMark.cpp:198；zBarrier.inline.hpp:626–660 |
| 保留observed及原槽，fast→make_load_good→current验证→slow→color→self_heal | Heap/z/zBarrier.inline.hpp:18 | zBarrier.inline.hpp:319–344；:72–79 |
| old Strong及Finalizable慢路：old入本域，young返回null | Heap/z/zBarrier.cpp:295、323 | zBarrier.cpp:185–203、234–250 |
| young字段：young入young；old仅major roots入old，否则返回current供heal | Heap/z/zBarrier.cpp:278 | zBarrier.cpp:158–183 |
| remset扫描专用young slow/color，仍young的槽重新登记 | Heap/z/zBarrier.inline.hpp:161；Heap/z/zRemembered.cpp:180 | zBarrier.inline.hpp:681–684；zRemembered.cpp:578–593 |
| 原生finalizer权威槽mark期发现→root Finalizable barrier→field closure | Heap/z/zMark.cpp:527、569；Heap/z/zReferenceProcessor.cpp:50 | zReferenceProcessor.cpp:174–201、239–250；zBarrier.cpp:218–232 |
| 每工作线程刷mark stacks，完成mark后才分类/enqueue | Heap/z/zMark.cpp:579；Heap/z/zGeneration.cpp:757 | zMark.cpp:797–834；zReferenceProcessor.cpp:239–250 |

基础设施边界：Cangjie原生finalizer登记保存原始NativeSlot，缺Java FinalReference对象的discovered字段；按172100Z保留原storage及现有discoveredList，在原链CAS声明一次，不新增权威对象表。不把这个adapter写成整套Java引用处理已同形。old根现有MarkOopClosure仍归P08/P3，不在本条复制第二套公共根漏斗。

CLAIM: 字段的代判断位于公共屏障current验证之后，对old→young慢路返回null，从而不进入color/self-heal。
  METHOD: read
  EVIDENCE: runtime/src/Heap/z/zBarrier.inline.hpp:18；runtime/src/Heap/z/zBarrier.cpp:295；runtime/src/Heap/z/zBarrier.cpp:323；ZGC zBarrier.inline.hpp:319

## 删除清单与限定检索

删除TraceFinalizableRefField整条独立query/staging/heal路径及WCollectorTraceRefField旧HealSite；删除non-strong阶段DoResurrection调用/定义/声明；不保留literal/nonheap早退，不放宽ValidateCurrentValue。字段young调用统一从MarkBarrierOnYoungOopField进入slow分路。
机械检索和完整命中行在evidence/p2-resume/call-index.json。限定产品Heap/ObjectModel范围删除检索见final-git.json；全runtime/src仍含Windows历史exports.def的DoResurrection字符串，**不把全目录命中数写成0**。正向对照同一检索器能命中MarkFinalizableBarrierOnRoot、TraceRefField、MarkFromOldSlowPath。Windows导出表未在Linux产品构建中证明可用，记录限制，不偷偷改ordinal。

## 产品接线证明

产品修改前顺序表已提交：evidence/p2-resume/producer-consumer.md；最终机械调用索引call-index.json含命令、rc和整行。以下锚均为runtime/src。

| 顺序/消费者 | 产品锚 | 测试结果如何进入断言 | 断线证据 |
|---|---|---|---|
| WriteReference→StoreBarrier→buffer Flush→rs.Record→mark-start flip/previous→young相位RescanRememberedSet | Heap/z/zStoreBarrierBuffer.cpp:99→Heap/z/zGeneration.cpp:472→Heap/z/zRemembered.cpp:180 | 真实old预置槽经young周期后overwrite young child；取消child其他根；读取child/sentinel真实存活和原槽更新 | qualified-minor/remset8-producer与remset8-consumer |
| remset scan_field→RemsetBarrierOnOopField→young MarkObject→FollowYoungObject | Heap/z/zRemembered.cpp:180→Heap/z/zBarrier.inline.hpp:161→Heap/z/zMark.cpp:937 | 第一周期接管，重新remember后第二周期仍存活 | qualified-minor/rearm |
| old worker→对象/struct/array/full/range→TraceRefField→old barrier | Heap/z/zMark.cpp:214、261、278、1358→:195 | full真实GC及数组目标slot到访、mark位、后继sentinel；慢路独立受控输入层读返回值、槽字、young位图/工作栈 | qualified-slow、qualified-array、qualified-array-final、qualified-struct |
| young字段→from-young→major roots门 | Heap/z/zMark.cpp:937→Heap/z/zBarrier.cpp:278 | nonmajor old未marked仍heal；major同一类old目标marked | qualified-full/young-major |
| OnFinalizerCreated/原weak storage→MarkOldRootsTask→DiscoverFinalizableRoot→原discoveredList→Finalizable root/field | Heap/z/zMark.cpp:569、527→Heap/z/zReferenceProcessor.cpp:50 | mark-end断点读取holder/child/sentinel的live/strong/color/discovery；完成后读取enqueue | qualified-closure/final8-producer、final8-field |
| 重复发现→原链CAS一次声明→ProcessReferences | Heap/z/zReferenceProcessor.cpp:67、85 | ReferenceProcessor.FinalDiscoveryIsClaimedOnce读取产品状态；enqueue、Strong升级为独立控制 | qualified-duplicate |

真实输入：测试DSO只编测试源，不拼产品.cpp。对象来自MObject::NewObject/MCC_NewObjArray/MCC_NewArray，字段用产品WriteReference，注册用产品FinalizerProcessor，周期由RequestGC发起。产品SO/测试DSO的full nm --defined-only双向对照在build8/{product,test}-defined-nm.txt及final-evidence.json；产品函数在SO，测试DSO有p2入口作为阳性对照。每臂LD_DEBUG记录加载SO，sha256在启动前捕获于identity.sha256，bounds保持相同。

字段慢路层（advisor181659Z明确接受）在真实old.StartOldMark后的testYoungMarkStarted点，以old GCWorkers::Run执行专用任务，调用完整TraceRefField；输入槽来自真实store历史，未手改颜色、phase、current、bitmap或中间发现结果。任务尾真实FlushMarkStacks并验证TLS空，后续正常GC验证old sentinel follow。**它不冒充原始根任务的直接调用证据**；原始根图由full/closure/array测试另证。

三条自查：①没有重编或拼接被测产品副本；测试宏只开observer/访问，产品default仍正常构建。②慢路层只安排真实入口及输入时点，没有手喂mark/current/发现结果；少量既有组件fixture显式建立synthetic domain，不能用作真实相位接线证据。③切除真实producer与young相位consumer后，存活目标断言确实失败，独立根通过；没有增加入口无条件失败或改测试断言造红。

## 承重面清单与因果臂

所有路径以下列前缀为根：kkk2:/root/sym_cangjie_runtime_607_implement_r5684610492-build8/。每臂N=1，属于确定构造的功能样本，不做概率/性能外推。具体断言文本、rc、hash、lineage、前后uptime、core及wall见各qualified-*/<arm>/和本地final-evidence.json。

| 面 | 绿/刀/恢复rc | 精确失败集合 | 保留的阳性控制 |
|---|---|---|---|
| Strong old→young slow返回/heal | 0/2/0 | strong_young_slow_no_object_result、strong_young_slow_does_not_heal | old→old mark/工作条目、合法fast |
| Finalizable old→young slow返回/heal | 0/2/0 | 对应final_young_slow两项 | Finalizable old→old及Strong同入口 |
| 错把young目标mark/publish | 0/2/0 | slow_old_fields_do_not_publish_young_entries、slow_old_fields_do_not_write_young_bitmap | old发布、fast与槽字判据 |
| young→old major roots | 0/1/0 | young_old_major_marks | nonmajor不mark仍heal、young及old控制 |
| remset buffered producer | 0/2/0 | child/sentinel第一周期存活 | independent_young_root_control |
| remset真实young phase consumer | 0/2/0 | child/sentinel第一周期存活 | independent_young_root_control |
| remset重新remember | 0/2/0 | child/sentinel下一周期存活 | 第一周期接管、独立根 |
| partial array consumer（Strong/Finalizable各一组） | 0/2/0 | 全字段数、range末槽到达 | 首/尾对象domain及独立Strong；struct数组同刀仍通过 |
| Finalizable mark期发现producer | 0/5/0 | holder/child/sentinel live、颜色、发现数 | Strong图follow |
| Finalizable字段consumer | 0/3/0 | child/sentinel Finalizable live、child非Strong | Strong图follow |
| Final discovered链去重 | 0/1/0 | FinalDiscoveryIsClaimedOnce | ProcessEnqueue/StrongUpgrade两项各0/0/0 |

红臂达到目标后退出，避免后续无效对象状态遮住已执行断言。因此slow绿/恢复23项、红20项；remset绿/恢复27项、生产/消费红5项、rearm红19项；closure绿/恢复10项、红9项。差异来自目标阶段后的正常周期/后处理断言未继续执行，**不是同总项数**，不隐瞒。full三臂均62项，数组均7项；duplicate每次filter各1测试。所有目标通过时同样打印P2_ASSERT PASS，证明不是只从源码猜到达。

额外观测：reference-array full/range各1041槽；struct-array两ref成员共2082槽，Strong/Finalizable都到末槽。late registration在真实root snapshot后分别small/LARGE/pinned/已有old登记：本周期不再发现/入队，下周期young对象由young标记、old候选mark期发现并一次入队，20项观测通过。晚登记这一面本轮尚无独立精确红，**仅计运行观测，不借发现切刀宣称全部晚登记语义已验收**。

CLAIM: 当前产品SO的old字段slow拒绝young发布/heal，故意恢复错误返回或young发布后仅对应目标断言失败。
  METHOD: test
  EVIDENCE: runtime/tests/gc_unit/test_p2_field_barrier.cpp:539；kkk2:/root/sym_cangjie_runtime_607_implement_r5684610492-build8/qualified-slow/{green,slow-strong,slow-final,slow-publish,restored}/result.json
  N: 1 per arm
CLAIM: 同入口old→old mark/follow及合法fast作为阳性控制，未因slow故障刀被一并判红。
  METHOD: control-arm
  EVIDENCE: kkk2:/root/sym_cangjie_runtime_607_implement_r5684610492-build8/qualified-slow/slow-strong/result.json；qualified-slow/slow-final/result.json；qualified-slow/slow-publish/result.json
  N: 1 per arm
CLAIM: remset真实生产和young相位消费均承载child/sentinel存活结果，独立young根保持通过。
  METHOD: test
  EVIDENCE: runtime/src/Heap/z/zStoreBarrierBuffer.cpp:99；runtime/src/Heap/z/zGeneration.cpp:472；kkk2:/root/sym_cangjie_runtime_607_implement_r5684610492-build8/qualified-minor/{green,remset8-producer,remset8-consumer,restored}/result.json
  N: 1 per arm
CLAIM: Finalizable mark期发现/字段follow及discovered去重分别有产品故障刀和恢复结果。
  METHOD: test
  EVIDENCE: runtime/src/Heap/z/zMark.cpp:569；runtime/src/Heap/z/zBarrier.cpp:323；runtime/src/Heap/z/zReferenceProcessor.cpp:67；kkk2:/root/sym_cangjie_runtime_607_implement_r5684610492-build8/qualified-closure；qualified-duplicate
  N: 1 per arm

## 断线位置与源文件恢复

语义回退刀：slow-strong.diff、slow-final.diff、slow-publish.diff、young-major.diff、final-*-regression.diff。它们不冒称基线承重点刀。基线承重点：producer-cut.diff的zStoreBarrierBuffer.cpp rs.Record及consumer-cut.diff的DoYoungGarbageCollection RescanRememberedSet；后者属于真实phase清单。entry_cut_check使用冻结ec8与最终候选head，JSON见evidence/p2-resume/entry_cut_check.json。所有刀只在本棒独立scratch树，主候选未带刀；最终源恢复记录见final8-source-restoration.json和final-git.json。array-partial.diff另断真实ProcessEntry consumer，不代替基线phase刀。

## 双构型证明与产物身份

build8/default与testable均configure/build rc=0；cmake MRT_TESTABLE_INTERNALS分别OFF/ON（实际命令见远端configure日志）。两构型独立并发、每构型-j192，wall20/21s。restored8两SO身份与green一致。故障臂也各用相同双构型入口，最后remset两臂default/testable分别15/15s、16/17s。每次通过wf_kkk2共享构建槽，未绕锁。

- green/restored default runtime：77cd9710487a1c4a1a9f29c1a892d5563422a06e31cd1385db8b794828e54fbf
- green/restored testable runtime：af2ff5dece608856cbdd1725023194e0206bdab9df24ff9e355931c581222508
- bounds全部对应臂：f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883
- 各切刀runtime及同一测试ELF/DSO sha逐臂列于final-evidence.json；不混用不同source cohort。build6/7旧证据保留但最终表使用build8共同产品/ELF组。

宏开运行只外推功能存在及因果，不外推性能。独立场景最多3测试臂并行；核心预约早期112–135、续期48–63/80–87、最终16–39，各次config记录为准。两端uptime与预约回执另见local-timing.txt。unit并发由主控wf包装固定GC_UNIT_JOBS=64；如实记64，不冒称192。串行1项为runner已有共享状态隔离策略。没有>10min的本棒串行构建/测试步骤。

UNIT_DEFAULT_RC=0
UNIT_FILLER_RC=0
UNIT_OHOS_RC=1

日志分别kkk2:/root/sym_cangjie_runtime_607_implement_r5684610492-build8/unit-final-default/run.log、unit-final-filler/run.log、ohos/evidence/configure.log。default与filler为**相同default ELF SHA**（主ELF0ea2e08bd6a4b3efa654ff84bbe36661082274e115a9ec5b29687b5d78eb4b7d），filler加CJRT_HEAP_FILLER=0；不是testable冒充。两者527/527，publication_rc=0，wall5.511s。该总数只记录本runner实测，不设目录固定门槛。

testable rc=1，728项中724通过/4失败，publication_rc=0，wall7.581s；失败集合NativeRootCurrent.MajorSeed、PartialArray.EncodableRejectsAbsoluteOnlyAlignment（同时INCOMPLETE）、PartialArray.RelativeBaseRoundtrips、ColourCensus.PlainWriteFunnelFailsClosed。未改这四项，也未加known_failures。它们位于根/codec/颜色census范围，不能据本轮资料声称已定位全部根因或已证明与冻结基线相同。OHOS真实配置尝试因MARK_GENERATION_PROBE control缺schedule.h返回1，**未到运行，未计目标红臂**。

CLAIM: default/filler同一ELF与同一产品SO运行均rc0，testable及OHOS失败如实保留。
  METHOD: measure
  EVIDENCE: kkk2:/root/sym_cangjie_runtime_607_implement_r5684610492-build8/unit-final-summary.json；ohos/evidence/configure.log；final-evidence.json
  N: 1 per configuration

## 测试增删与失败分类

机械集合差见test-name-delta.json，基线ec8，含GC_TEST和托管extern入口；runner环境mode分支在上表逐轴展开。

- 新增真实托管Finalizable closure、late-registration、数组/struct及slow输入入口，新增FinalDiscoveryIsClaimedOnce。
- TraceRefFieldRemapsPreviousRelocationEpoch改名LoadBarrierRemapsPreviousRelocationEpoch：其holder/target为young，所测是load remap，不是old mark字段；仍验证原current与slot remap并增加返回检查（ZGC zBarrier.inline.hpp load barrier）。
- 既有Remset及InPlaceRemsetMovesBitAndFeedsConsumer fixture在新公共barrier下需绑定本地collector/domain，从产品domain取实际pending work；原remember/rearm/移动/工作条目断言保留。明确属组件fixture，不冒充真实GC producer。
- MarkPort203 major数组fixture缺正常young prelude中的old color flip；按ZGC zGeneration.cpp:1213–1226补fixture mark-start输入，保留四项原数组断言。
- 没有为过绿删除有效测试、减断言、常量false、禁用分支或放宽CHECK。

## FALSIFIED

1. “old字段碰young必返回null”未区分快慢路：真实remset已着mark-good色可合法fast返回current且不改槽。advisor174802Z/175006Z按ZGC明确null约束只针对slow；独立slow层保持该断言并有回退刀，没强制全慢。
2. 原fresh-old raw-null首次store测试未建立remember历史，实际失活输入已保留；责任经advisor归P05 #615/P08 #614，不能在本条删ZGC raw-null fast-path。新图明确用真实old→old预置、young周期、overwrite建立有效历史。
3. late LARGE分配下一周期仍可留young，不能预期四种都进old Finalizable；按真实当前代断言young消费者或old发现，不改产品代判定。
4. 原MarkStripeStack storage observer反映容量分配，不证明每次条目publish；该实验观测已移除，最终数组证据用真实字段结果及FollowPartialArray consumer故障刀。
5. 原spawn/终结触发程序遇String.myData非heap producer。没有恢复literal早退；主控另立cjcj#48，不能据简化main通过宣称原始输入通过。

## 尚未闭合/接续

- 必须消费#48独立审查并合入的compiler/std/ELF，按172829Z复跑原spawn和finalizer_trigger；届时保留同次maps/load-bias/holder TypeInfo/slot offset、两SO及编译链身份。当前真实原失败资料保留在本棒早期managed/backtrace及finalizer-trigger目录。
- 晚登记新增时间轴目前只有运行观测，没有独立精确红；后续补因果刀时不能用更早CHECK遮住目标。
- testable四失败及OHOS配置问题不伪装全绿；按现有根/P01/基础设施归属接续，未私扩产品范围。
- 尚未做独立Review；本报告的闭环自评只覆盖上表明确具备三段证据的面。原始协程输入整包闭环仍不成立。

SYM-PR: cangjie-runtime#631
