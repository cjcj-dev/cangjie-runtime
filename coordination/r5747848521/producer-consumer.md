待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标基于 1a185f30814d37aedc864c379ccb6c3e992a6ae5；本轮只修 A1/A2 测试，不增加产品观测接口。

| 承重面 | producer → consumer 顺序 | ZGC |
|---|---|---|
| 分段根注册 | FinalizerProcessorTest::Queue / RegisterFinalizer / Heap::RegisterExportRoot → OopStorage slots | gc/shared/oopStorage |
| young 根消费 | ZDriver::RunGarbageCollection → ZGenerationYoung::pause_mark_start / concurrent_mark(:455) → produceYoungRoots(:472) → ZMark::VisitMinorRoots(:252) → ZWorkers::run(:279) → MarkYoungRootsTask::work(:234) → RootsIteratorAllColored::Apply(:136) → MarkYoungGoodBarrierOnOopField → slot CAS | zMark.cpp:852-891; zGeneration.cpp:665-692 |
| export 的额外消费者 | ZMark::VisitMinorRoots 的 uncolored closure 还调 VisitAllExportRoots(:273) → MarkBarrierOnOopField | 既有路径；断线矩阵必须覆盖，不能只切 colored 变体后宣称全域 |
| RawRemap | RunMajorRawRemap → ZDriver::RunGarbageCollection → ZGenerationOld::collect → concurrent_remap_young_roots(:1154) → ZRelocate::RemapYoungRoots → raw slot update → 测试读取根/字段/派生根 | zGeneration.cpp:1509-1522 |

本轮移除 RawRemap 测试块 MRT_GC_UNIT_TESTS 外层条件。根分段测试逐槽保存原始带色字，以真实 GC 后每一槽的颜色转换作结果；后置遍历只用于读取结果，不能把遍历次数称为 GC 消费次数。断言前打印 transitioned/values_valid，生产断线与消费断线必须令目标失败，恢复后同 ELF 回绿。最终实测结果另附。
