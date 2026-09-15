PROGRESS=DONE · verdict=R1 pinned 快捷页退休修复及因果验证完成，送独立Review ｜尺=产品SO定向五臂＋套件集合差 N=各臂各用例1 · LANE=sym_cangjie_runtime_606_implement_r5675167676
DELIVERY_REF=cangjie-runtime|sym/606-implement-r5673376405|afce09cd22b05a70195f240baf971f2113ebcaa0
SIDE_EFFECT: 原候选分支/PR #628；保留已知红，不授予合并许可
ROLE=implement
EVIDENCE=local:/root/cj_build/reports/EVIDENCE-sym_cangjie_runtime_606_implement_r5675167676,kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final

LANE=sym_cangjie_runtime_606_implement_r5675167676
ROLE=implement
PROGRESS=DONE

## 范围与坐标

本轮仅修上一轮审查 R1：pinned 快捷页在所属代 mark-start 退休。产品提交 afce09cd22b05a70195f240baf971f2113ebcaa0；在原分支继续，不改 main。
指定仓 `/root/cj_build/cangjie_runtime` 独立回读 cjcjdev/main，rc=0，实际 fb7d5282867aaa3b9d8b5df2f6691227ff3424ce；冻结栏 38c7b2a4fad2d72ef001b07669f1f1b7f434f9a6 是上一轮候选。advisor 052802Z答复确认此记账（`/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/advisor-answer.md`）。两次 fetch/merge 均 rc=0、Already up to date；本轮增量与 cut 用38c7，主线内容核用fb7d。

P1此前位图、mark入口/consumer、birth、old PostTrace selection修复承接原候选，不重复宣称本轮实现。上一轮证据见 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r2/REPORT.md` 与其原始 kkk2 路径。P2/P3过渡接口仍按原报告归属，本轮不扩大范围。

## ZGC 对应与 producer→consumer 顺序

Z=`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`；下表我方短路径以 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/runtime/src/Heap/z/` 展开。

|生产/消费次序|我方位置与动作|ZGC锚/约束|
|---|---|---|
|分配入口→首次快捷尝试|ObjectModel/MObject.cpp:28 → zObjectAllocator.hpp:31/37 → AllocPinnedLocked:26|zObjectAllocator.cpp:238，每龄分配入口|
|新页身份→首次发布|zObjectAllocator.hpp:54 TakeRegion默认old → :77生命周期列表 → :78 per-age pinnedPage → :79 bump|zPage.cpp:90–112，先age/owner再reset_seqnum；zObjectAllocator.cpp:63–110|
|竞争者第二次快捷尝试|zObjectAllocator.hpp:64，在同一个列表mutex内读同一个pinnedPage；已有页成功则归还未用新页|保留分配竞争成功/失败两支，不用列表头作快捷权威|
|所属代 mark-start退休|zGeneration.cpp:141 → zObjectAllocator.cpp:263/266，对传入每龄清pinnedPage及shared页|zGeneration.cpp:1222 → zObjectAllocator.cpp:196/224：退休快捷页，不选择候选|
|seq→phase→domain|zGeneration.cpp:150/152/158，退休先于owner序号递增|zGeneration.cpp:1231–1237|
|标记期间再次分配|zObjectAllocator.hpp:28读取已退休的快捷页，因此TakeRegion新页；birth==owner.seq|zPage.inline.hpp:180–186|
|mark后生命周期消费者|zRelocationSet.cpp:63 → selector的old∧relocatable选择|zGeneration.cpp:205；不把selection搬回mark-start|

数据形态：每龄allocator的独立原子页指针，mark-start按龄清空，与shared快捷页同一退休函数。pinned的地址稳定及资源列表仍是Cangjie基础设施差异，不把独立pinned对象接口标为ZGC同形；这项差异不豁免退休分路。

CLAIM: pinned分配权威已从生命周期列表头迁到每龄快捷页，并由所属代退休步骤清空。
  METHOD: read
  EVIDENCE: /root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/runtime/src/Heap/z/zObjectAllocator.hpp:21；/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/runtime/src/Heap/z/zObjectAllocator.hpp:28；/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/runtime/src/Heap/z/zObjectAllocator.cpp:266；/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zObjectAllocator.cpp:196

## 删除清单与主线内容核

本轮删除 AllocPinnedLocked 从 recentPinnedRegionList.GetHeadRegion 分配的路径；其定义移到能看到PerAgeObjectAllocator定义的zObjectAllocator.hpp。生命周期列表、SlotList资源清理、PostTrace selection保留。没有新增显式mark、过滤、CHECK豁免或旧槽分配口。
限定域检索：`rg -n 'pinnedPage|AllocPinnedLocked|RetireSharedPages|NewPinnedObject|testOldMarkStarted' runtime/src`，完整命令、rc和逐条原文在 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/source-checks.json`。旧水位/AllocPinnedFromFreeList删除检索在同文件；其原基线阳性证据承接delivery-r2/source-checks-final.json，不以本轮38c7已经删除的内容伪造阳性。
`git grep -c`逐文件主线/HEAD对照在 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/main-content-counts.json`：

|特征|主线→候选|
|---|---|
|MarkYoungGoodBarrierOnOopField|3→3|
|MarkYoungRootObject|7→7|
|MarkYoungRootsTask|5→5|
|PushHeapRoot|5→5|
|PublishThreadRoot|3→3|
|ColorAddressMarkYoungGood|2→2|
|RemapYoungRoots|20→20|
|basic_oop_iterate_safe|13→13|

## 双构型证明

固定入口 `/root/cj_build/ops/bin/kkk2_build_two.sh <本树> sym_cangjie_runtime_606_implement_r5675167676-final`；default/testable并行，实际-j192，各configure/build rc=0/0，wall=52s/52s。宏关保持产品退休/分配正确性；新增测试观察callback仅在MRT_TESTABLE_INTERNALS内，实例布局未改变。

|构型|变量|build rc|runtime SHA256|
|---|---|---|---|
|default|-DMRT_TESTABLE_INTERNALS=OFF|0|39ac2af8aa73e28877344902e815d14de3077aee8d39af5180d91c8dd4944e21|
|testable|-DMRT_TESTABLE_INTERNALS=ON|0|44d8fc64c6c2c4303c9819e25e28aa48280430ec3060f465e0fab88832c0bf9f|

bounds均 f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883。原始日志 `kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/{default,testable}-build.log`；本地摘要 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/final-build.log`。三个独立故障臂也通过同入口同时构建两构型（六臂并行，-j192，wall50–54s），日志为 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/{retire,consumer,cut}-build.log`。所有步骤未超过10min。

## 常态三栏及本包名单

UNIT_DEFAULT_RC=1
日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/unit-default.log；533项，532通过，唯一失败 ValueRootCurrentization.NullAndNonHeapControlsRemainStable。
UNIT_FILLER_RC=1
日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/unit-filler.log；同一default两枚ELF，CJRT_HEAP_FILLER=0，533项，532通过，同一失败。
UNIT_OHOS_RC=128
日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/ohos-cwdfix/unit.log；OHOS-host configure/build rc=0/0，runner读取归档.git身份失败128，未运行用例，不算通过。真实尝试配方见 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/run-ohos.sh`。

Testable：729项，728通过，唯一失败同上；这项上一轮已按P3归属，本轮不改断言/加豁免。本包P1窗口与原pinned回收控制均通过。套件差集见 `kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/comparison/suite-deltas.json`，不是用固定PASS数凑绿。

## 产品接线证明

|测试|真实产品函数|结果进入断言|断开后|
|---|---|---|---|
|P1Mark.PinnedMarkStartRetiresAllocationPage|MObject::NewPinnedObject → AllocPinned；RequestGC → DoYoungGarbageCollection → StartOldMark → RetireSharedPages；DoTracing:1204仅暂停窗口|/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/runtime/tests/gc_unit/test_segmented_array_init.cpp:956–964读取实际after页、birth/owner/phase/位图；:975合并判据，:1294断言|retire、consumer、cut均rc=1；green/restored rc=0|
|P1Mark.PinnedReclaimedSlotIsNotAllocationSource|同分配入口，完整major之后再分配；selector/资源清理原路径|/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/runtime/tests/gc_unit/test_segmented_array_init.cpp:1000的新页/implicit live与存活根观察|退休刀rc=1；consumer回退仍0，证明原用例确实漏窗口|
|YoungWeakClosure.SingleWorkerKeepsYoungReferentStrong|DoYoungGarbageCollection真实VisitMinorRoots调用|原有P1_FIELD_FOLLOW_ASSERT读产品闭包结果|基线cut rc=1，其余0|
|P1Mark.DuplicateAnyThreadStopsAtConsumer|真实MarkEntryObject消费路径|原有consumer reached/live断言|所有臂0，独立控制|

三条自查：①没有在测试内重编/拼产品组件，full `nm --defined-only`显示NewPinnedObject、RetireSharedPages、DoTracing均在产品SO，测试ELF不定义它们；ELF的main及SO的产品函数互为检索阳性，见 `kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/symbols.json`。②未手工喂birth、phase、mark、current或队列；通过真实分配/GC请求建立状态，callback只阻塞并发窗口。③删退休承重点确实红；原产品CHECK未更改，所有窗口臂打印目标状态后才完成断言，没有更早CHECK遮住。

闭环自评：R1生产端与消费端 ✅。真实产品身份、实际页状态进入断言、对应产品刀改变判词三段均有产物。此自评仅描述证据闭环，不替代独立Review。

## 红臂、恢复与身份

正式定向记录 `kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/targeted/`；每臂每测试N=1，命令/rc/wall在results.json；同一testable主ELF sha256 `aea845334e55e04e80531c42733d99a158df9a28d943be75e1dde7c47eca0817`，第二ELF及runtime+bounds的完整哈希、CJRT-COMMIT源内容stamp、CJRT-DECLARED提交、核域在各臂identity.json。测试taskset 0–31，独立臂/用例并发；套件GC_UNIT_JOBS=192。正式定向两端uptime：`/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/local-targeted-before.txt`、`local-targeted-after.txt` 与 `kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/targeted/uptime-before.txt`、`uptime-after.txt`。较早全套比较的远端uptime也在comparison，但本机首次记录在比较完成后，故不拿它充作比较前的本机负载证据；正式定向重新记录完整时间包围，不重跑套件取绿。

|臂|产品刀|窗口目标rc|目标打印different/current/birth/owner|控制：回收/重复consumer/young字段|
|---|---|---|---|---|
|green|无|0|1/1/1/1|0/0/0|
|retire|zObjectAllocator.cpp:266去掉pinnedPage清空（retire.diff，回退刀）|1|0/0/0/1|1/0/0|
|consumer|zObjectAllocator.hpp:28恢复生命周期列表头分配（回退刀）|1|0/0/0/1|0/0/0|
|cut|zGeneration.cpp:141删基线退休调用；:347断VisitMinorRoots基线相位调用|1|0/0/0/1|1/0/1|
|restored|恢复使用原始保留SO|0|1/1/1/1|0/0/0|

所有窗口臂reuse=1、no_bitmap=1、trace=1；因此红来自跨周期新分配页身份，不来自丢失测试输入、显式mark或错误相位。精确输出和FAIL位置见各臂 `P1Mark.PinnedMarkStartRetiresAllocationPage.log`。生产/消费承重面各一刀；候选新增退休行明确记为regression，不假冒基线行。

基线cut实物 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/cut.diff`，retire及consumer回退分别为 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/retire.diff`、`/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/consumer.diff`。entry_cut_check对38c7基线、afce09候选rc=0，JSON `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/entry_cut_check.json`，两处删改行均基线逐字存在，真实相位入口DoYoungGarbageCollection命中。基线cut额外根入口断线承接P1已有产品测试，不冒称只有pinned一个承重面。

全套每臂N=1、同runner与同两枚ELF：green/restored 728/1；retire 726/3（新增两项pinned）；consumer 727/2（只新增窗口）；cut709/20（新增pinned两项+17项原根/字段接线测试）。逐项集合在suite-deltas.json；未把广泛根断线红冒充pinned精确红。基准与恢复的失败集合相同，定向判词相同，未作性能/耗时优劣推论。

产物生成时SO hash由固定构建入口立即记录；测试使用保留产物，green=restored，三个故障SO各异；bounds及两ELF五臂相同。配对sodepot路径为 `/root/sodepot/<runtime完整sha256>/`，清单 `kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/sodepot.json`。默认/filler复用同ELF，由run-unit.sh直接调用同路径保证，未把testable当filler。

## 承重面清单

机械命令及逐条原文：`/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/source-checks.json` 的producer-consumer。每一条调用分别确认如下，完整外围历史面承接delivery-r2/producer-consumers-final.json。

|消费者/分支|产品锚|臂与结果|
|---|---|---|
|首轮缓存命中/失败→TakeRegion|zObjectAllocator.hpp:37/54|窗口同周期reuse阳性、新周期new-page；retire/consumer红|
|锁释放后第二次缓存尝试/新页发布|zObjectAllocator.hpp:64/78|代码接同一个AllocPinnedLocked；本轮未构造双mutator争抢，竞争CAS/锁胜负覆盖不新增宣称，既有资源路径保留|
|young按龄退休不清old|zObjectAllocator.cpp:265＋zGeneration.cpp:75|完整major先young再old；窗口同周期复用/旧完整用例为控制；未单独新增young-only pinned测试|
|old按龄退休→新周期分配|zObjectAllocator.cpp:266＋zGeneration.cpp:141|targeted/{green,retire,consumer,cut,restored}窗口rc=0/1/1/1/0|
|PostTrace生命周期选择/下一old周期根保留|zRelocationSet.cpp:63；原pinned回收用例|原用例在green/consumer/restored通过；退休刀失败；不改变selection职责|

上述未新构造的双mutator竞争、young-only pinned面明确为覆盖限制；修法统一作用在相同每龄快捷指针，不以未执行面宣称完整T1–T4重新验收。本轮针对Review R1给定的真实窗口闭环完成。

## 测试增删

相对38c7，仅新增 `P1Mark.PinnedMarkStartRetiresAllocationPage`，未删除/改名/修改原有效断言。机械集合差 `/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5675167676/delivery-r3/test-name-delta.json`。原回收用例作为控制保留；观察口宏关不存在但正确性代码仍在。

## FALSIFIED

冻结表把候选38c7写成main，与指定仓回读不符；advisor已确认为渲染口径问题，未改变工作范围。上一轮原pinned测试仅在整轮后观察，因此consumer回退仍通过而新窗口失败；这个差分实测支持R1，不把原用例删除。

## 后续项与边界

没有新拆分问题。沿用既有P3 ValueRootCurrentization红和OHOS归档身份限制；本报告不授予含缺验结果的合并许可。没有修改共享SDK、官方远端或其他棒工作树。形式校验结果后附；实质由独立Review判。

SYM-PR: cangjie-runtime#628

CLAIM: 真实pinned分配在old mark-start窗口符合当前birth，撤销生产端退休或恢复旧消费端分别精确转红，恢复通过。
  METHOD: test
  EVIDENCE: runtime/tests/gc_unit/test_segmented_array_init.cpp:960；kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/targeted/results.json；kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/targeted/retire/P1Mark.PinnedMarkStartRetiresAllocationPage.log
  N: 1 per arm per test

CLAIM: 同周期页复用、未显式发布位图与真实TRACE窗口在故障臂仍成立，重复consumer独立控制通过。
  METHOD: test
  EVIDENCE: runtime/tests/gc_unit/test_segmented_array_init.cpp:960；kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/targeted/consumer/P1Mark.PinnedMarkStartRetiresAllocationPage.log；kkk2:/root/sym_cangjie_runtime_606_implement_r5675167676-final/targeted/consumer/P1Mark.DuplicateAnyThreadStopsAtConsumer.log
  N: 1 per arm per test

形式检查：cjops deliver check rc=0（form-check.log）；只代表形式检查通过，实质由Review判。
