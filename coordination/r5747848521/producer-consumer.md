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
