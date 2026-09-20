待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标基于 1a185f30814d37aedc864c379ccb6c3e992a6ae5；本轮只修 A1/A2 测试，不增加产品观测接口。

| 承重面 | producer → consumer 顺序 | ZGC |
|---|---|---|
| 分段根注册 | FinalizerProcessorTest::Queue / RegisterFinalizer / Heap::RegisterExportRoot → OopStorage slots | gc/shared/oopStorage |
| young 根消费 | ZDriver::RunGarbageCollection → ZGenerationYoung::pause_mark_start / concurrent_mark(:455) → produceYoungRoots(:472) → ZMark::VisitMinorRoots(:252) → ZWorkers::run(:279) → MarkYoungRootsTask::work(:234) → RootsIteratorAllColored::Apply(:136) → MarkYoungGoodBarrierOnOopField → slot CAS | zMark.cpp:852-891; zGeneration.cpp:665-692 |
| export 的额外消费者 | ZMark::VisitMinorRoots 的 uncolored closure 还调 VisitAllExportRoots(:273) → MarkBarrierOnOopField | 既有路径；断线矩阵必须覆盖，不能只切 colored 变体后宣称全域 |
| RawRemap | RunMajorRawRemap → ZDriver::RunGarbageCollection → ZGenerationOld::collect → concurrent_remap_young_roots(:1154) → ZRelocate::RemapYoungRoots → raw slot update → 测试读取根/字段/派生根 | zGeneration.cpp:1509-1522 |

本轮移除 RawRemap 测试块 MRT_GC_UNIT_TESTS 外层条件。根分段测试逐槽保存原始带色字，以真实 GC 后每一槽的颜色转换作结果；后置遍历只用于读取结果，不能把遍历次数称为 GC 消费次数。断言前打印 transitioned/values_valid，生产断线与消费断线必须令目标失败，恢复后同 ELF 回绿。最终实测结果另附。

## A2 控制结果
kkk2:/root/sym_cangjie_runtime_627_implement_r5747848521/root-controls-v2/results.json：Strong、WeakFinalizer、Export 各 N=3。green/restored rc=[0,0,0]，producer/consumer rc=[1,1,1]；RegionAge.YoungAgeRoundTrip 四臂均 rc=[0,0,0]。目标断言前输出 slots=1536 remaining=1536，正常 transitioned=1536，两刀 transitioned=0，values_valid 四臂均为 1。缺页/加载错误均不计为转红；首轮使用 uncolor 的早期断言遮挡已在 v2 改为不自愈的 GetTargetObject。

producer.diff 断在 ZGenerationYoung::concurrent_mark 中唯一 produceYoungRoots() 调用；consumer.diff 断 RootsIteratorAllColored::Apply 的 strong/weak.apply，及 VisitMinorRoots 内第二个 export barrier。两刀 entry_cut_check rc=0，JSON 与 diff 同目录。独立 SO 两構型均构建 rc0（-j192，两构型并行）；仅 testable ELF 含三项 RootStorageSegments，因此此控制矩阵不外推 default 测试执行。

## 本轮测试增删（相对 1a185f30）
没有删用例、改名或新增套件。RawRemap 十项从未编入恢复为实际注册（publication ELF）；RootStorageSegments 三项改为读取产品槽转换，原有 family/分段规模保留。后置遍历恰好一次只验证读取器覆盖，不宣称产品对每槽只调用一次屏障（幂等屏障无法从最终槽字推出这一点）。

## A1 真实输入修正与 T 类裁决
1. `ResetDeliveryUnit` 仅构造页元数据；RawRemap 自己的 source/destination/second old page 通过既有 `PublishAllocatedPage` → `Heap::alloc_page` 发布，不改共享夹具。初始 N1 用例在实际产品查页处失败，GDB 日志保留于 evidence/r5747848521/diagnose/。
2. 原生根 vector 追加可能更换槽地址（Mutator.cpp:330-350）。通过公开 AddNativeFrameRoot/PopNativeFrameRootsTo 先建立本帧容量，再取得槽；用 PopNativeFrameRootsTo 统一释放，避免逐项 erase 使其它槽地址改变。
3. mutator 注册必须早于 young relocate 色翻转，其 saved load-good color 才对应 from 输入（ZGC zStackWatermark.cpp:155-173）。保留原普通根/null/nonheap/派生根/栈对象/无头记录断言。
4. 实际 `ZGenerationOld::mark_start` 取代 synthetic sequence+`ZMark::Start`：后者只初始化栈，不发布 Mark 相位；ZGC zGeneration.cpp:1213-1242 的 producer 同时处理两者。
5. 主控 `old-pending-ruling.md` 授权三项按 T 类修观察时点。driver 后的 OLD_PENDING_TARGET_ASSERT 是该阶段的产品结果目标：转发表产生非 identity winner，原生/派生槽尚未扫描、仍保持 before。之后真实 `Mutator::GcPhaseEnum(false)` 才进入最终 RAW_REMAP_TARGET_ASSERT。新旧名逐项见 test-changes.tsv；没有删除断言、常量豁免或增加产品回调。

## A1 断线分路
- raw-producer.diff：ZDriver::RunGarbageCollection 的 old.collect 调用。七个 young-source 场景在最终 RAW_REMAP_TARGET_ASSERT 红；三个 oldPending 场景应在 driver 阶段 OLD_PENDING_TARGET_ASSERT 红（不能借它证明下一根扫描）。
- raw-consumer.diff：Mutator::GcPhaseEnum 的 PushHeapRoot 消费调用，证明七个 young-source 的真实根处理。
- raw-nextscan.diff：同一基线调用仅在非 Mark 相位省略，保留 old 标记/选择/复制。三个 oldPending 场景必须先通过 deferred=1，再在最终槽目标断言红；七个 young-source 是反向对照。
- FindToPublicState.NotManagedIsObservable 同 ELF 的独立产品判定，在所有臂均应通过；最终结果按实测 JSON 记录，不把整包全红作为有效证据。
