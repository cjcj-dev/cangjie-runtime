待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。

坐标基于冻结主线 `7e95511ac96ea63354cac97789b827b68986e11f`。

# #527 testable 产品与 standalone 的编译契约

`run_standalone.sh:218-223` 见分段数组钩子后给测试定义 `MRT_GC_UNIT_TESTS`。因此 testable SO 必须编入相同的完整观测与类型布局；只导出单个钩子不足以满足契约。`runtime/CMakeLists.txt:118-119` 在任一测试构型启用时为产品定义该宏，但不改变 `MRT_GC_UNIT_TESTS` CMake 布尔，不额外构建 in-tree 套件。默认构型保持宏关闭。

| 观测/布局 | 生产端 | 消费端 | 基线 testable 产品 → 候选 |
|---|---|---|---|
| 分段数组 hook 与结果记录 | ObjectModel/MArray.cpp:29,60,96 | gate_gc_unit.sh:298；MArray.cpp:203 断言 | GC_UNIT_TESTS 未定义 → 定义 |
| 分段数组分配入口 | ObjectModel/MArray.cpp:65 | ObjectModel/MArray.inline.h:137 | 同上 |
| 根访问记录 | MArray.cpp:73,84 | Mutator.cpp:371,931；zMark.cpp:518,571,597；zRelocate.cpp:831；BaseObject.cpp:105；MArray.cpp:232 | 同上 |
| Ghost 观测 | Heap/Allocator/zPage.cpp:125-147；zPage.hpp:464,584 | zPage.inline.hpp:905；test_ghost_region_lookup.cpp:57,67 | 同上 |
| ClearLiveInfo Young/Old 实例 | zPage.cpp:287-292 | test_live_map.cpp:44-45,379,424 | 同上 |
| StoreBarrier flush 观测 | StoreBarrierBufferTestObservations.h:10,23；zStoreBarrierBuffer.hpp:41,75 | zStoreBarrierBuffer.cpp:76,80,100；test_store_barrier_buffer.cpp:533 起 | 同上 |
| GCDriverPort waitingReceipts | zDriverPort.hpp:90 | zDriverPort.cpp:118,131；CollectorResources 内两个 port | 同上，避免成员偏移不一致 |
| CollectorResources 测试成员 | zDriver.hpp:126 | zDriver.cpp 的 GC_UNIT_TESTS 观测；gc_unit_main.cpp:29 访问后续 collectorProxy | 同上，避免成员偏移不一致 |
| 其余原有同宏定义 | `rg -n 'MRT_GC_UNIT_TESTS' runtime/src` | 同一产品编译定义传递到所有目标，测试原本定义同宏 | 同上；无逐源码宏重命名 |

所有源码内部宏保持原样；不把测试为 friend 临时定义的 `MRT_TESTABLE_INTERNALS` 当成产品布局事实，不删除任何钩子或测试。

ZGC 机制参照（不是测试API对应物）：`zObjArrayAllocator.cpp:92-110` 发布不可枚举数组头与 invisible root；`zForwarding.cpp:86-108` retain page；`zLiveMap.inline.hpp:37` liveness reset；`zStoreBarrierBuffer.cpp:199-222` phase mark/remember。本包不更改这些机制。

裁定：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_527_implement_r5661089766-20260914T084631Z.md`。导出合同由真实 gate 守卫撤宏红臂验证；托管行为失败与既有 finalizer 失败分别按 B13、B12 记录，不能由守卫证据推导行为通过。

## 同批最小类型适配

`runtime/src/Heap/WCollector/WCollector.cpp:150` 读取 `it->first.object` 再转 `ExportObject*`。这是 B09 ValueRoot 包装迁移漏改，GC_UNIT_TESTS 分支此前不编译故未暴露；同函数 `:158` 已使用相同解包。授权 `/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_527_implement_r5661089766-20260914T085545Z.md`。不改变回调或 GC 流程；撤回此行的证据仅证明编译适配，不作为 GC 行为转红。
