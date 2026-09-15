from pathlib import Path
import json
root=Path.cwd();d=root/'delivery-r2';r=json.loads((d/'final-evidence-summary.json').read_text())
remote=r['root'];lane='sym_cangjie_runtime_606_implement_r5674249495';head='9de9226b8d58ed655534b195df2f6938be789062';base='fb7d5282867aaa3b9d8b5df2f6691227ff3424ce'
report=f'''PROGRESS=DONE · verdict=R1/R2与主线接线修正完成，送独立Review；保留已知红 ｜尺=产品SO单元＋P1阶段四臂 N=每单元臂1/每阶段臂3 · LANE={lane}
DELIVERY_REF=cangjie-runtime|sym/606-implement-r5673376405|{head}
SIDE_EFFECT: 仅候选分支与本棒产物；复用PR #628；不授予带已知红的合并许可
ROLE=implement
EVIDENCE=local:{d},kkk2:{remote}

LANE={lane}
ROLE=implement
PROGRESS=DONE

## 交付结论与坐标

本报告记录实现与验证，不代替独立Review。产品/测试源坐标为 `{head}`；交付文档提交若在其后，runtime内容必须相同。正式交付头以 `/root/cj_build/reports/REPORT-{lane}.md` 为准。

初读指定仓 `/root/cj_build/cangjie_runtime` 的 cjcjdev/main，rc=0，得 `c3973505c171aa7e72095c3357ffd57f56156707`；任务表的 c9e91240 是上轮候选。advisor 033048Z已纠正。返工原分支，合入 #596 后基线为 `{base}`；merge提交 c46468bc0，冲突仅zMark.cpp的include，保留Generation和Barrier两侧。最后fetch/merge各rc=0、Already up to date，见 `final-main-sync.json`。三点diff、cut检查均用新基线，不用祖先关系猜内容。

裁决：`/root/cj_build/ops/advisor/outbox/{lane}-20260915T033048Z.md` 要求常态运行；033239Z限定类归属/夹具；035006Z批准selection时机修正；040345Z批准P1专用托管runner与#629装置归属；041513Z按Q59-B分开P1目标断言和#607后续字段故障。没有observer Abort、提前退出、删CHECK或放宽输入资格。

## ZGC对应与producer→consumer顺序

C=`{root}/runtime/src/Heap/z/`；Z=`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`。下表短锚按这两个根展开。初始顺序表/机械检索在 `order.md`、`producer-consumers.txt`；最终全量命中含命令/rc/原文在 `producer-consumers-final.json`。

|面|ZGC锚|最终C锚/顺序|边界|
|---|---|---|---|
|strong/finalizable原语|zBitMap.inline.hpp:55-83；zLiveMap.inline.hpp:100|zLiveMap.inline.hpp:41/68/80，strong CAS成功true、重复false；finalizable返回incLive|升级不重复live结算；不是页层取反伪迁|
|页包装/旧enqueue|zPage.inline.hpp:284|zPage.inline.hpp:478/507/541/614直接传newlyMarked；:646只在旧EnqueueObject边界取反|该旧P3接口仍返回already，未改route位图职责|
|young start|zGeneration.cpp:855-880|zGeneration.cpp:53：:62颜色→:74 usage reset→:75 shared退休/:76 TLAB退休→:97 seq→:99 phase→:105 domain→:113 remset|ResetTLABUsage不再夹带退休；真实TLAB当前/预备项均观察|
|old start|zGeneration.cpp:1212-1237|zGeneration.cpp:124：:133颜色→:141退休→:150 seq→:152 phase→:158 domain|无selection夹在mark-start|
|合并入口|zGeneration.cpp:601-602|zGeneration.cpp:221 young完整start→:224 old完整start|minor不推进old|
|qualified young根接回|zGeneration.inline.hpp:118-129；zMark.inline.hpp:48-87|#596 barrier→zGeneration.cpp:195 MarkYoungRootObject→GenerationCycle::MarkObject→MarkDomain::MarkObject|只接#596已建立current的输入；删除无剩余调用者的MarkRootObject中转，不把裸根全局强转|
|页birth/other|zPage.cpp:90-112；zPage.inline.hpp:180-186|zPage.cpp:319 ResetPageSequence；zPage.inline.hpp:1468/1473|birth==owner为allocating，birth<owner为relocatable；保留other快照|
|对象入口/条目|zMark.inline.hpp:48-87|zMark.inline.hpp:16：allocating→GCThread claim/AnyThread query→resurrected→offset+四位→publish=!GCThread|partial数组仍独立；不吞follow|
|消费者|zMark.cpp:403-432|zMark.cpp:1015/1424→:1643 MarkEntryObject|entry.mark失败停；GCThread已claim条目合法；incLive结算后才follow|
|old selection|zGeneration.cpp:205-225|zRelocationSet.cpp:63，mark后select/collect前；zRelocationSetSelector.cpp:66/72/102/155按old+relocatable选小/LARGE/pinned|三组Assemble删ClearLiveInfo；保留本轮标记结果和资源释放职责|
|owner守卫|zRelocationSet.cpp:79-134|zRelocationSet.inline.hpp:66→Heap/Allocator/zForwardingTable.cpp:115|删除晚期剔young修补；原owner CHECK直接验证selector结果|

类归属仍为获准过渡：GenerationCycle组合现有collector依赖，未造第二套代状态；由P14a把类/Phase/ZAbort搬入正式每代对象。Cangjie侵入式分配列表、TLAB统计与预备页退休是现存分配基础设施；本报告不把这些类的整体形态标为已完成，也不越入P11b分桶/flip_age/promote barrier。

CLAIM: R1统一了实际位图原语返回约定，升级与首次live职责分离。
  METHOD: read
  EVIDENCE: {root}/runtime/src/Heap/z/zLiveMap.inline.hpp:41；{root}/runtime/src/Heap/z/zLiveMap.inline.hpp:80；/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zBitMap.inline.hpp:55

CLAIM: R2每代启动顺序已集中到对应成员，old selection在标记完成后。
  METHOD: read
  EVIDENCE: {root}/runtime/src/Heap/z/zGeneration.cpp:53；{root}/runtime/src/Heap/z/zGeneration.cpp:124；{root}/runtime/src/Heap/z/zRelocationSet.cpp:63

## 删除清单与后续接口

产品限定域 `runtime/src`：markStartAllocPtr、InitializeAllocationWatermark、AllocatedAfterMarkStart、RegionIsAllocatingPage、ExemptMarkStartAllocatingFromCSet、AllocPinnedFromFreeList均由统一birth/入口/consumer替代。命令、候选rc=1与基线rc=0阳性对照在 `source-checks-final.json`。删除的是水位资格、重复young follow、pinned旧槽分配与晚期selection修补；没有删除pinned资源释放/SlotList暂存、地址稳定约束、FROM冻结身份或独立route位图。

额外删除：#596合入的MarkRootObject仍引用旧水位并使用旧返回约定；全树仅声明、定义和一个消费者，消费者已迁Generation入口，故删除中转。三组Assemble的ClearLiveInfo退出selection；ClearLiveInfo本体实际只解除seal，不是清位图，报告不把它误述为即时位图清零。

P2待迁：TraceRefField、PushYoungObject的字段/Y2y来源、YoungStripedMarkingWork::PushObject/PushFilteredYoung及相关裸对象生产。P3待迁：旧MarkOldObjectIfActive/MarkYoungObjectIfActive、PublishThreadRoot/value/seed剩余适配和EnqueueObject旧合同。它们的限定检索见最终清单；不声称保留这些接口就是全树mark形态完成。#596已获资格的young-good槽链单独接回统一Generation入口；#603 task/storage与#498帧历史色不在本包。

CLAIM: 已删除的水位/旧槽分配符号在候选的产品限定域无匹配。
  METHOD: read
  EVIDENCE: local:{d}/source-checks-final.json（同一git grep表达式，候选rc=1）
CLAIM: 同一删除检索在基线有匹配，作为上述阴性结果的阳性对照。
  METHOD: read
  EVIDENCE: local:{d}/source-checks-final.json（基线{base}，rc=0）

## 主线内容核

`main-content-counts-final.json` 保存逐条 `git grep -c` 命令、rc、逐文件输出。#596的MarkYoungGoodBarrierOnOopField 3→3、MarkYoungRootObject 7→7、MarkYoungRootsTask 5→5、PushHeapRoot 5→5、PublishThreadRoot 3→3、ColorAddressMarkYoungGood 2→2；#595 RemapYoungRoots 20→20；#599 basic_oop_iterate_safe 13→13，其有效skip分路保留。

宽泛的ZIterator::总数37→36：唯一差集是任务明确要求删除的young重复mark仍follow分支（基线zMark.cpp中wasMarked分支的oop_iterate）；正常follow仍走MarkPartialArray::FollowObjectReferences。不能为凑计数恢复重复遍历。这一减量单列为P1规定删除，不把它伪报成非减。

## 双构型证明

固定入口：`/root/cj_build/ops/bin/kkk2_build_two.sh {root} {lane}-final2`。摘要 `final2-build.log`，远端default/testable-configure.log和build.log。两构型独立并行、实际-j192；各configure/build rc=0，wall=53s/52s。观察静态回调只在MRT_TESTABLE_INTERNALS启用，宏关的产品mark、birth、selection和CHECK仍在。

|构型|CMake变量|构建rc|runtime SO sha256|
|---|---|---|---|
|default|-DMRT_TESTABLE_INTERNALS=OFF|0|906bd29176296396bdb9d2f0baab9ea5f5297ff62cc4ff13e58f591ffb6ca323|
|testable|-DMRT_TESTABLE_INTERNALS=ON|0|3a8c39fc0d8e54f91df05d30aa6ed13cb995c30d281504bd08feb76c84aded3b|

bounds两构型均 f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883。sodepot双配置配对归档见 `sodepot-manifest.json`；测试使用构建完成时保留的原配对，不事后重建取哈希。所有构建/用例wall均在日志；没有超过10分钟的顺序构建步骤，不作性能结论。

## 常态套件结果

UNIT_DEFAULT_RC=1
日志 kkk2:{remote}/unit-default.log；533项，532通过，1失败。
UNIT_FILLER_RC=1
日志 kkk2:{remote}/unit-filler.log；533项，532通过，1失败。CJRT_HEAP_FILLER=0，调用同一default的两枚ELF；创建时哈希见 `default-filler-identity.txt`。
UNIT_OHOS_RC=128
日志 kkk2:{remote}/ohos-cwdfix/unit.log；独立OHOS-host configure/build rc=0/0，runner在归档缺.git的血缘读取退出128，未执行用例，不算OHOS通过。构建配方/产物hash/真实尝试记录均保留。

Testable standalone：728项，727通过，1失败，rc=1。三种常态单元臂唯一失败都是 `ValueRootCurrentization.NullAndNonHeapControlsRemainStable`，按既有裁决归P3；NullControlRemainsStable等原有效断言保留并通过。未加known_failures/过滤或修改非法输入CHECK。

## 产品接线证明

|测试/入口|实际产品承重点|产品结果进入断言|断线结果|
|---|---|---|---|
|P1BitMap.StrongClaimResult/FinalizableClaimResult|zLiveMap.inline.hpp:41/80（由产品zLiveMap.cpp定义）|首次/重复返回值；独立内容/live控制|分别只破对应返回合同，控制仍通过|
|YoungWeakClosure.SingleWorkerKeepsYoungReferentStrong/StripedKeepsYoungReferentStrong|DoYoungGarbageCollection zGeneration.cpp:347→VisitMinorRoots→实际root task→worker字段闭包|P1_FIELD_FOLLOW_ASSERT读取实际closure的referent/child|producer刀在该行禁用调用，目标字段断言失败；没有前置root断言遮住它|
|P1Mark.DuplicateAnyThreadStopsAtConsumer|zMark.cpp:1015→MarkEntryObject:1643|真实consumer输出reached及live|consumer刀忽略claim结果，reached 1→2，live仍16|
|P1专用托管runner|MObject::NewObject/export根/RequestGC→zGeneration.cpp:53/124|产品回调交本代实际domain，读取颜色、TLAB、seq、phase、domain、remset|old/young顺序刀命中各自P1目标，四臂各3发|
|9个major入口用例|PostTrace selection→BeginForwardingArena owner CHECK|实际selected-list资格与标记/闭包结果|提前selection造成跨代混入，9项在原owner CHECK精确转红；恢复回到仅P3失败|

三条自查：①被测组件没有在测试中重编/拼装；`symbol-definitions-final.json`：7个直接核心定义在产品SO，单元ELF/托管observer/托管ELF的同名核心定义为0，main/p1MarkStartExercise阳性在场（full nm rc=0）。②P1托管runner不手工喂中间mark/current/phase/queue状态；核心位图及合成页用例的装置例外见下节，不能冒充完整根链。③基线真实producer、consumer各断一刀都确实红；组合刀也实际构建运行，不能只用静态patch作证明。

## 承重面清单与红臂

完整消费者原文/调用命令见 `producer-consumers-final.json`；符号/产物与逐项差集见 `final-evidence-summary.json`。对象分支轴：young/old、AnyThread/GCThread、Follow/DontFollow、strong/finalizable、allocating/下一所属代、另一代推进、LARGE/普通/partial、single/multiworker。直接位图调用的无返回值route消费者zRelocate.cpp:508/562保留原职责。未迁P2/P3入口与完整T1–T4所有真实分配/CAS分支不作超出已有产物的闭环宣称。

所有下表单元臂N=1，testable、同两枚ELF/同runner；未变产物逐字节相同，green=restored runtime，cut运行时SO不同。完整列表在 `comparison/suite-deltas.json`；新增失败集合而非固定PASS数作差分。

|臂|全套通过/失败（总728）|相对基准新增失败|目标/控制|
'''
for arm,row in r['comparison'].items():
 c=row['counts'];desc={'green':'仅既有P3失败','restored':'与green同一失败集','cut-strong':'StrongClaimResult红；FinalizableClaimResult和内容/live控制通过','cut-final':'FinalizableClaimResult红；StrongClaimResult和内容/live控制通过','cut-producer':'single/striped字段follow目标红','cut-consumer':'重复消费reached=2目标红','cut-mixed-selection':'9项均命中owner CHECK','cut-combined':'两端故障集合并集，字段follow与duplicate均红'}[arm]
 report+=f"|{arm}|{c[1]}/{c[2]}|{len(row['new_failures'])}|{desc}|\n"
report+=f'''
全套每臂rc=1，不能称全套绿；定向合同的green/restored rc=0，对应刀下rc=1。strong原语被广泛消费，所以全套80项新增失败是其真实依赖面，不把这80项说成“仅一项红”；精确返回合同的三项小集分别是1红/2控制通过。producer17项、consumer7项组合恰为24项新增失败，完整名单及每项日志保留。

CLAIM: 位图返回合同有精确阳性/阴性对照，内容与live控制没有随返回故障被破坏。
  METHOD: test
  EVIDENCE: kkk2:{remote}/comparison/cut-strong/P1BitMap.StrongClaimResult.log；kkk2:{remote}/comparison/cut-final/P1BitMap.FinalizableClaimResult.log；kkk2:{remote}/comparison/restored/P1BitMap.ClaimContentsAndUpgradeControl.log
  N: 1
CLAIM: 基线producer调用与consumer结果各自影响目标字段闭包/重复消费断言，恢复回到基准。
  METHOD: test
  EVIDENCE: kkk2:{remote}/comparison/cut-producer/YoungWeakClosure.SingleWorkerKeepsYoungReferentStrong.log；kkk2:{remote}/comparison/cut-consumer/P1Mark.DuplicateAnyThreadStopsAtConsumer.log；kkk2:{remote}/comparison/restored/P1Mark.DuplicateAnyThreadStopsAtConsumer.log
  N: 1
CLAIM: 混入另一代页被原owner CHECK拒绝，而恢复臂保留本包9项正常major结果。
  METHOD: control-arm
  EVIDENCE: kkk2:{remote}/comparison/cut-mixed-selection/suite.log；kkk2:{remote}/comparison/restored/suite.log；local:{d}/final-evidence-summary.json
  N: 1

`final2/cut-combined.diff` 就是实际构建运行的两端断线补丁：zGeneration.cpp:347的VisitMinorRoots调用、zMark.cpp:1015的MarkEntryObject结果；两行均逐字存在于基线。`entry-cut-final.json` rc=0，入口DoYoungGarbageCollection，不拿候选新行冒充基线。其他顺序/返回/selection刀标为regression，各自diff和双构型build日志在final2/。瞬态破坏只在本棒scratch副本；候选产品最终hash见 `product-source-hashes-final.json`，runtime工作区无未提交改动。

## P1托管四臂：阶段验收与整程资格分栏

按041513Z，验收对象是mark-start回调内的14个目标断言名，机械导出在 `p1-four-arms/expected-phase-assertions.json`。每条每次PASS/FAIL及可见输出在各arm/0、1、2的run.log和result.json。每发允许有多个young start，不把一发内重复打印当独立样本。observer只交实际domain指针供只读观察，不是判据来源；判据来自ZGC顺序及产品状态。

|臂（每臂N=3）|P1阶段目标|缺到达项|整程真实状态|
|---|---|---|---|
|green|14个名字全部PASS|0|三发SIGABRT，subprocess rc=-6（shell表示134）；后续#607字段故障|
|old-order-cut|仅old_color_before_retire FAIL|0|同一后续#607故障；不得写整程通过|
|young-order-cut|仅young_remset_unchanged_before_domain、young_remset_unchanged_after_domain_start FAIL|0|同一后续#607故障|
|restored|14个名字全部PASS|0|同一后续#607故障|

四臂同一托管ELF与observer，只有product runtime变化，bounds不变；每个重复编号固定核域0-31/32-63/96-127，四臂不换该编号核域。所有12发都有完整LOADFC行，target StateWord=3的MARKSTALE日志和阶段完成输出，见 `final-evidence-summary.json` 的phase_runs。完整RequestGC返回后的后置断言和最终P1_MARK_START_RESULT未到达，明确不在已获证范围。没有截断、observer写Abort、或按预期错误退出当0。

CLAIM: P1阶段的顺序回退只改变对应目标判词，恢复后目标全部通过，四臂均真实经过产品start入口。
  METHOD: control-arm
  EVIDENCE: kkk2:{remote}/p1-four-arms/results.json；kkk2:{remote}/p1-four-arms/green/0/run.log；kkk2:{remote}/p1-four-arms/cut-old-order/0/run.log；kkk2:{remote}/p1-four-arms/cut-young-order/0/run.log；kkk2:{remote}/p1-four-arms/restored/0/run.log
  N: 3
CLAIM: 同一托管输入在P1阶段之后进入old字段trace并触发已登记#607的current资格守卫，未取得整程通过。
  METHOD: measure
  EVIDENCE: kkk2:{remote}/p1-four-arms/results.json（12条均含完整LOADFC记录、rc=-6）；local:{d}/final-evidence-summary.json
  N: 每臂3

## 原managed装置结果（#629）

保留run_generation_cycle_context.sh及全部原ASSERT；N=3均CYCLE_RC=134 / SATB_RC=139，runner整体rc=1，日志 `kkk2:{remote}/managed-0.log`、managed-1.log、managed-2.log。finalizer夹具直接塞队列但不更新调度谓词，命中hasFinalizableJob与queue一致性CHECK；SATB自有main不支持标准other-vm子进程filter参数。两者按#629记录为装置缺陷，不计产品红臂。已修的仅旧API、真实collector accessor、compiler宿主runtime选择与测试helper链接，未给其产品CHECK开口。

## 夹具例外与测试增删

合成页测试不能调用完整StartYoungMark来退休真实全局分配列表：它们替换FDM/heap span，或只构造mock Collector。所有手推seq只存在测试 `gc_cycle_sequence_fixture.cpp`，一个独立测试TU通过唯一宏friend访问；产品没有AdvanceSequence入口。不能将这些用例外推为真实GC相位接线。

|例外消费者|受影响测试/用途|为何不能冒充真入口|
|---|---|---|
|gc_heap_fixture.hpp AdvanceGeneration/AdoptGenerationIdentity|LiveMap/RegionBitmap、页birth与carrier等合成heap用例，包含P1 AllocatingAndRelocatablePolicyMatrix|植入页表、旧对象与collector替换，验证页/对象原语，不拥有真实分配列表|
|mark_publication_fixture.hpp|P1 DuplicateAnyThreadStopsAtConsumer/ResurrectAndInactivePhasePolicies、GenerationMark SATB、mark条目用例|先建隔离mark域与synthetic span；只claim consumer及条目合同|
|clear_entries_product_unit.cpp|合成forwarding/owner/route入口套件|保持生成器页身份，不将helper的seq作为真实producer证明|
|test_mark_port_203_entries.cpp、test_gc_request_sync.cpp|旧代mark条目、mock request-port|前者测条目/计数，后者mock无真实WCollector分配域|
|test_remset.cpp|按代序号与两面remset纯合同|fixture提供独立remset，不能用全堆remset替代它|

当前三条R1原语测试的bitmap初始化是输入构造，不是手工喂mark结果；实际MarkBits来自产品。真正phase顺序闭环由独立P1托管runner承担。T1全部真实A→B根/字段组合、T3所有CAS胜负及T4所有资源类型仍不能靠合成夹具宣称全面闭环；未迁面依任务归P2/P3，不新增豁免。

测试名称集合差相对新基线见 `test-name-delta-final.json`：新增9项、删除1项；删除LiveMap.MarkStartAllocWaterIsImplicitLive由PageBirthSequenceIsImplicitLive替代，所属P1删除水位机制。R1的旧false=first期望按ZGC改成true=first，位图内容/live/重复断言保留；字段follow目标断言放到root收据前，原收据断言仍在。新增专用managed源/runner不在GC_TEST宏集合中，另行实际运行并留四臂证据。

## 身份、核域与证据边界

单位/托管ELF、observer、每臂runtime与bounds的SHA256和源码章在 `final-evidence-summary.json`，7个核心符号full nm及main/p1MarkStartExercise阳性在 `symbol-definitions-final.json`。局部测试观测构型不能外推性能。两端uptime：本地final-local-uptime-before/after.txt，远端comparison与p1-four-arms的before/after；原始读数也汇入summary。

构建默认/testable并行，各-j192；故障8臂分别双构型并行；全套runner GC_UNIT_JOBS=192，独立测试臂并行，托管12发并行。预约核域由cjops windows取得，日志/metadata与实际taskset一致；load只作过载判断，不据此做速度或性能判词。

## FALSIFIED

1. 冻结表main=c9e91240错误，实际是上轮候选；独立回读及033048Z修正已列。
2. 旧Assemble可以留在mark-start并非无害假设：young→old顺序下新增9项owner CHECK失败；mark后selection修复后9项恢复，再前移则同9项转红。
3. 初版managed装置用旧API、跨DSO内联getter读域结果不可靠，不能据此断言产品domain没启动；实际本代domain指针回调显示workers匹配。原#629装置与后续#607字段问题分开归档。
4. 初期producer刀所选小集全绿，不能作接线证据。最终断DoYoungGarbageCollection真实VisitMinorRoots调用，单/多worker字段闭包目标均红，组合刀实际运行且过entry_cut_check。
5. 不把ZIterator总数降低一处误记为丢失#599：差集是规定删除的重复follow分支，未用无意义命中填数。

## 后续与收尾

#607：P1之后old字段屏障current资格已知红，完整输入/LOADFC/三发证据已按041513Z交主控；#629：两项旧managed装置缺陷；P14a：类归属；P2/P3/#603：剩余接口/根task/storage。上述均已有归属，不重复开issue。进入Merge是否允许带#607已知红，必须Review后由主控另取绑定最终Delivery-ref的Merge-exception，本实现不自授。

形式检查、最终push与外部正式头在收尾追加；形式检查rc=0只表示报文结构，不代替独立Review。

SYM-PR: cangjie-runtime#628
'''
(d/'REPORT.md').write_text(report)
print('report lines',len(report.splitlines()))
