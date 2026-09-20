待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。

坐标：4cc8e34adb7fc3379c4680ab894b4ce5da4d8aca；仅本轮窄范围删除。

| 文件 | 旧定义 / 消费 | ZGC 对应物 | 归属与处置 |
|---|---|---|---|
| Heap/Allocator/zPage.cpp | GhostLookupTestHook；仅孤立 Allocator/CMakeLists.txt 列入源表 | 无；实际页实现对应 zPage.cpp:33，remset 验证 :153,159 | #627 删除整文件；保留 Heap/z/zPage.cpp |
| Heap/Allocator/zPageAllocator.cpp | RecentFullAccounting、MRT_ALLOCATION_STALL_OBSERVE 旧定义；仅孤立源表 | 无；实际分配入口对应 zPageAllocator.cpp:1401-1418 | #627 删除整文件；保留 Heap/z/zPageAllocator.cpp |
| Heap/Allocator/CMakeLists.txt | add_library(Allocator)、旧测试宏；上层未 add_subdirectory | 无独立 Allocator 静态库对应 | #627 删除整文件；保留 Heap/CMakeLists.txt |
| Heap/Allocator/RegionSpace.cpp、CartesianTree.cpp 及其头/依赖 | 现役 Heap/CMakeLists.txt:42,52 | 分配适配层，#727/#730 已裁归属 | 原地保留 |
| Heap/shared | 当前 gc/shared 消费内容 | HotSpot gc/shared | 原地保留 |

本轮不改测试；旧 Ghost 测试文字引用不被误记为产品消费者。本轮不以删用例解释 BASE-ONLY。
