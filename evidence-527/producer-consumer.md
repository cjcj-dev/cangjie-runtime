待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest

坐标基于 7e95511ac96ea63354cac97789b827b68986e11f。

|生产端|消费端|顺序/修改点|
|---|---|---|
|ObjectModel/MArray.inline.h:138 分配测试入口|ObjectModel/MArray.cpp:65 分配钩子|编译宏必须一致|
|ObjectModel/MArray.cpp:96 托管测试启用与 invisible root 发布|Mutator/Mutator.cpp:371,931 根枚举；Heap/z/zMark.cpp:518,571,597；Heap/z/zRelocate.cpp:831；Common/BaseObject.cpp:105|所有观测端与生产端同构型编译|
|以上根访问记录|ObjectModel/MArray.cpp:203 required/forbidden 断言|产品结果进入托管测试断言|
|ObjectModel/MArray.cpp:60 导出钩子|tests/gc_unit/gate_gc_unit.sh:298 守卫|门需要 TESTABLE 构型具备钩子|

ZGC: /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zObjArrayAllocator.cpp:92,110 发布不可枚举数组头与 invisible root；现役机制，不适用删除分支。

## Advisor 083441Z 授权补齐现役观测宏

|钩子|生产端|消费端|改前→改后|
|---|---|---|---|
|GhostLookupTestHook|Heap/Allocator/zPage.cpp:125-147；Heap/z/zPage.hpp:464,584|Heap/z/zPage.inline.hpp:905；tests/gc_unit/test_ghost_region_lookup.cpp:57,67|MRT_GC_UNIT_TESTS → MRT_TESTABLE_INTERNALS|
|ClearLiveInfo Young/Old 实例|Heap/z/zPage.cpp:287-292|tests/gc_unit/test_live_map.cpp:44-45,379,424|产品实例门控 MRT_GC_UNIT_TESTS → MRT_TESTABLE_INTERNALS；测试仍用 suite 宏引入 extern template|
|SetFlushObserverForTest|Heap/Barrier/StoreBarrierBufferTestObservations.h:10,23；Heap/z/zStoreBarrierBuffer.hpp:41,75|Heap/z/zStoreBarrierBuffer.cpp:76,80,100；tests/gc_unit/test_store_barrier_buffer.cpp:533 起|MRT_GC_UNIT_TESTS → MRT_TESTABLE_INTERNALS|

按该裁定不删除钩子或测试。对应 ZGC：zForwarding.cpp:86-108 的 retain page；zPage.inline.hpp:180-185 的 mark-start liveness；zStoreBarrierBuffer.cpp:199-222 的 phase flush mark/remember。这里修测试观测形态，不改这些机制。
