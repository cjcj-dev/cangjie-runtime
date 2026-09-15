PROGRESS=DONE · verdict=P1已实现，定向绿/切刀/恢复证据已归档；完整套件保留P3失败，OHOS运行未通过 ｜尺=run_standalone N=每臂1 · LANE=sym_cangjie_runtime_606_implement_r5673376405
DELIVERY_REF=cangjie-runtime|sym/606-implement-r5673376405|ed85a7dc240a4675a5bf5b1ed2c78e585d060740
SIDE_EFFECT: 候选产品与同批测试提交；未合并主分支
ROLE=implement
EVIDENCE=local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5673376405/delivery, kkk2:/root/sym_cangjie_runtime_606_implement_r5673376405-accept-delivery

LANE=sym_cangjie_runtime_606_implement_r5673376405
ROLE=implement
PROGRESS=DONE

## 范围与坐标
冻结仓 /root/cj_build/cangjie_runtime 的 cjcjdev/main 回读 rc=0：c3973505c171aa7e72095c3357ffd57f56156707。本轮再次 fetch/merge rc=0，Already up to date。仅P1；不宣称P2/P3根字段资格已迁入。P1接口见 /root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5673376405/delivery/P1-interface.md；生产消费先后见 /root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5673376405/delivery/P1-order.md。主线包逐符号内容计数见 main-content-counts.json，查询命令见 source-search-commands.json。

## 对应表与删除清单
|产品|ZGC锚（/root/cj_build/reference/jdk/src/hotspot/share/gc/z/）|动作|
|zPage.cpp ResetPageSequence；zPage.inline.hpp:1467|zPage.cpp:90-112；zPage.inline.hpp:180-186|页birth及other序号；allocating/relocatable按owner序号|
|zGeneration.cpp:120|zGeneration.cpp:855-880、1212-1237|退休快捷页后推进所属代周期|
|zMark.inline.hpp:16|zMark.inline.hpp:48-87|typed入口，allocating先退；独立resurrect/gcThread/follow/finalizable轴|
|zMarkStackEntry.hpp|zMarkStackEntry.inline.hpp|heap offset及独立控制位，partial独立|
|zMark.cpp:979|zMark.cpp:403-432|consumer竞争失败返回，去重复follow|
|zObjectAllocator.cpp pinned分配|zObjectAllocator.cpp:238-249|删除旧槽分配及分配时显式mark；保留资源清理|

已删除对象水位出生资格、carrier额外live、类型并集allocating判断、selector补救及pinned旧槽分配族。限定产品检索见 deleted-product-symbols.txt；不能以空输出代替行为证明，阳性臂见下表。裸指针MarkYoungObjectIfActive/MarkOldObjectIfActive、PushYoungObject/PushFilteredYoung及根种子包装保留给P2/P3，逐调用归属见 legacy-callers.txt；不将过渡接口标为形态一致。

## 双构型证明
固定入口 kkk2_build_two.sh；default MRT_TESTABLE_INTERNALS=OFF / testable=ON；两臂并行，-j192。接回主线后的构建各 rc=0，wall=49s；摘要 /root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5673376405/delivery/recovery-build-summary.log。该构建后仅测试夹具修正，产品源码未变；切刀因果比较使用保留的accept-delivery产品SO，不混用带不同commit stamp的recovery产品。构建日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5673376405-recovery/。测试可证明功能，不外推性能。

UNIT_DEFAULT_RC=1
日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5673376405-accept-delivery/unit-default.log：527项/526通过/1失败。
UNIT_FILLER_RC=1
日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5673376405-accept-delivery/unit-filler.log：同一default构型，CJRT_HEAP_FILLER=0，527项/526通过/1失败。
UNIT_OHOS_RC=128
日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5673376405-accept-delivery/ohos/build.log（build rc=0）、ohos/unit.log（runner rc=128）。真实尝试--gc-unit-ohos-host；归档树缺.git导致runner血缘读取失败，未获得OHOS运行验收。
Testable修正夹具后日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5673376405-accept-delivery/unit-recovery.log：717项/716通过/1失败，rc=1。唯一剩余ValueRootCurrentization.NullAndNonHeapControlsRemainStable绑定P3；advisor 021104Z明确要求保留，未改断言，未自授合并许可。此失败绑定本报告DELIVERY_REF；合并例外须另行登记。

## 产品接线证明
产品符号通过dlsym调用产品模板实例；身份哈希见 recovery-evidence-summary.json identities，完整SO血缘见远端 control-recovery/*/identity.json。green/restored使用同一保留SO；所有故障臂使用同一unit-recovery ELF及boundscheck，只有对应runtime SO变化。每臂N=1。定向12项用于目标断言到达；全套切刀结果用于差集。
|测试/面|产品调用|目标日志/切刀|结论边界|
|P1Mark.AllocatingAndRelocatablePolicyMatrix|zMark.inline.hpp:16|focused-recovery/cut-alloc；P1_ALLOCATING_ASSERT|产品入口结果进入断言；合成页矩阵，不替代全部真实分配入口|
|P1Mark.DuplicateAnyThreadStopsAtConsumer|zMark.cpp:979|focused-recovery/cut-consumer；P1_CONSUMER_ASSERT|产品consumer结果；受控发布后消费，不宣称根资格完整|
|MarkAllocation两项|zGeneration.cpp:120 FlushAllocationRegions|focused-recovery/cut-retire|真实GC路径；两项目标资格断言转红|
|P1Mark.PinnedReclaimedSlotIsNotAllocationSource|zObjectAllocator.cpp pinned分配|focused-recovery/cut-pinned|实际分配结果进入断言|
|SharedSmallPage.AgeRefillAndRetirement|shared页退休|focused-recovery/cut-shared|退休后页身份结果|
|ForwardingPublicationProduct.CompactRegionDeadFromHasNoForwardingAndIsNotTlab|CompactRegion目的页reset|focused-recovery/cut-inplace|FROM/TO身份结果|

证据根均为 kkk2:/root/sym_cangjie_runtime_606_implement_r5673376405-accept-delivery。切刀补丁 /root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5673376405/delivery/cut-*-delivery.diff，构建日志摘要同名-build-summary.log。基线切刀验证 entry-cut-delivery.json rc=0；consumer刀含基线PushYoungObject发布及consumer claim两处，不能把它视作独立单面刀。纯语义回退刀不冒称基线调用点刀。

## 承重面清单
机械检索全行输出：page-producer-consumers.txt、mark-producer-consumers.txt、legacy-callers.txt；命令与rc为 source-search-commands.json。完整清单文件保留供审查，不以命中数代替调用语义。
定向集每臂12项：
- green: pass=12；失败=；单项rc=1，其余rc=0。
- cut-alloc: pass=11；失败=P1Mark.AllocatingAndRelocatablePolicyMatrix；单项rc=1，其余rc=0。
- cut-retire: pass=10；失败=MarkAllocation.LargeHolderAndNewTargetAreImplicitlyLive,MarkAllocation.LargeHolderKeepsRootedExistingTargetLive；单项rc=1，其余rc=0。
- cut-consumer: pass=10；失败=P1Mark.DuplicateAnyThreadStopsAtConsumer,LargePageGeneration.ArrayRootKeepsYoungTargetLive；单项rc=1，其余rc=0。
- cut-pinned: pass=11；失败=P1Mark.PinnedReclaimedSlotIsNotAllocationSource；单项rc=1，其余rc=0。
- cut-shared: pass=11；失败=SharedSmallPage.AgeRefillAndRetirement；单项rc=1，其余rc=0。
- cut-inplace: pass=11；失败=ForwardingPublicationProduct.CompactRegionDeadFromHasNoForwardingAndIsNotTlab；单项rc=1，其余rc=0。
- restored: pass=12；失败=；单项rc=1，其余rc=0。

全套各臂rc均为1（包含P3既有失败）；计数及相对恢复新增失败：
- cut-alloc: [717, 715, 2]；新增=['P1Mark.AllocatingAndRelocatablePolicyMatrix']
- cut-retire: [717, 713, 4]；新增=['MarkAllocation.LargeHolderAndNewTargetAreImplicitlyLive', 'MarkAllocation.LargeHolderKeepsRootedExistingTargetLive', 'TLABUsage.AllocationCycleResizesNextRefill']
- cut-consumer: [717, 697, 20]；新增=['LargePageGeneration.ArrayRootKeepsYoungTargetLive', 'LoadHealDeliveryProduct.InPlaceRemsetMovesBitAndFeedsConsumer', 'MarkPort203Entries.LegacyParallelCollectionConsumesArrayTails', 'MarkPort203Entries.ParallelInvisibleThenNormalAccountsOnce', 'MarkPort203Entries.ParallelNormalThenInvisibleAccountsOnce', 'MarkPort203Entries.SerialCollectionConsumesArrayTails', 'MarkPort203Entries.SerialCollectionHandlesExactArrayThreshold', 'MarkPort203Entries.SerialCollectionHandlesOnePastArrayThreshold', 'MarkPort203Entries.SerialInvisibleThenNormalAccountsOnce', 'MarkPort203Entries.SerialNormalThenInvisibleAccountsOnce', 'MarkPort203Entries.StripedCollectionConsumesArrayTails', 'MarkPort203Entries.StripedInvisibleThenNormalAccountsOnce', 'MarkPort203Entries.StripedNormalThenInvisibleAccountsOnce', 'MarkPort203Entries.StructArrayCollectionVisitsBothFields', 'MarkPort203Storage.YoungCollectionReturnsActualSegments', 'P1Mark.DuplicateAnyThreadStopsAtConsumer', 'YoungWeakClosure.OldWeakSlotKeepsYoungReferentStrong', 'YoungWeakClosure.SingleWorkerKeepsYoungReferentStrong', 'YoungWeakClosure.StripedKeepsYoungReferentStrong']
- cut-pinned: [717, 715, 2]；新增=['P1Mark.PinnedReclaimedSlotIsNotAllocationSource']
- cut-shared: [717, 715, 2]；新增=['SharedSmallPage.AgeRefillAndRetirement']
- cut-inplace: [717, 715, 2]；新增=['ForwardingPublicationProduct.CompactRegionDeadFromHasNoForwardingAndIsNotTlab']
- restored: [717, 716, 1]；新增=[]

闭环自评：⚠ 整包证据仍有边界。真实分配/GC面有产品结果与切刀因果证据；typed入口矩阵采用受控页，consumer测试采用受控发布，不能外推完整真实根/字段闭环或所有CAS竞争分支。保留12项定向证据，不谎称全部T1–T4分支已闭合。OHOS runner缺口亦不计为验收通过。

## 测试增删
集合差见 test-name-difference.json。新增6项；LiveMap.MarkStartAllocWaterIsImplicitLive替换为LiveMap.PageBirthSequenceIsImplicitLive，原因是水位资格按P1被删除，以ZGC页seq语义测试替代。其余有效断言保留。共享夹具按advisor 020049Z授权复用/迁移collector周期身份；NativeRootCurrent直接Trace夹具补old周期推进（ZGC zGeneration.cpp:1212），前版MajorSeed提前命中CHECK，修后目标正常执行；未在产品开豁免。

## FALSIFIED
前序把合成夹具周期不一致推为正常产品路径被挡已撤回，advisor 020049Z确认。此次MajorSeed读证与实测也显示直接Trace绕过mark-start周期推进；修夹具后全套差集只减少此失败（unit-testable.log与unit-recovery.log），没有弱化产品CHECK。

CLAIM: 页birth与typed每代入口已写入产品，裸根适配保留P2/P3边界。
  METHOD: read
  EVIDENCE: /root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5673376405/runtime/src/Heap/z/zMark.inline.hpp:16；/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5673376405/runtime/src/Heap/z/zPage.inline.hpp:1467
CLAIM: allocating故障注入使目标矩阵失败，恢复后相同定向集通过。
  METHOD: test
  EVIDENCE: kkk2:/root/sym_cangjie_runtime_606_implement_r5673376405-accept-delivery/focused-recovery/cut-alloc/P1Mark.AllocatingAndRelocatablePolicyMatrix.log；同目录green/restored；recovery-evidence-summary.json
  N: 1
CLAIM: allocating负向断言有下一周期同入口发布的阳性对照。
  METHOD: test
  EVIDENCE: /root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_606_implement_r5673376405/runtime/tests/gc_unit/test_young_conc.cpp:1095；kkk2:/root/sym_cangjie_runtime_606_implement_r5673376405-accept-delivery/focused-recovery/green/P1Mark.AllocatingAndRelocatablePolicyMatrix.log
  N: 1

## 证据身份与负载
SO/ELF产生时哈希和两端uptime见recovery-evidence-summary.json及远端每臂identity.json。核域预约0-31见core-window-focused.txt；定向臂taskset 0-31，8臂并行；全套runner GC_UNIT_JOBS=192，7个产品臂并行，各约8秒。所有结果仅单发功能证据，不作统计或性能结论。产物不混用重建SO。

## 最终目标断言核验
远端 evidence/product-nm-defined.txt 为被测SO完整 nm --defined-only；mark-entry-symbols.txt 为入口实例摘录。cut-alloc 日志在test_young_conc.cpp:1098：pending=1对期望0；cut-consumer在:1149：reached.size=2对期望1，均有前置产品结果输出。见 focused-recovery 对应单项日志。

## 交付状态
本轮按协议允许的“证据不足”如实交独立Review；不自审放行，不宣称整包运行闭环成立。P1代码、测试、回退臂均保存，后续Review须特别核验上述分支覆盖限制。报告形式检查rc=0仅代表形式。
