待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标：开工主线 ed74b464efc257cc884efd94d5ee923a970b6eab，初始候选 f93dd83044f2860fbe116bae0ee7176100973729。

| 顺序 | 生产/消费 | 修改前锚 | 处置 |
|---|---|---|---|
| 1 | old mark 收尾 PostTrace | zGeneration.cpp:991 → zRelocationSet.cpp:68 | 删除重复 CollectLargeGarbage 路；由既有 selector 统一处理空页 |
| 2 | CollectLargeGarbage 写 Garbage | zPageAllocator.cpp:991 → zPageAllocator.inline.hpp:52 | 删除唯一产品 Garbage 生产者；不另造即时释放分路 |
| 3 | old relocate / OOM / finalizer 异步消费 | zRelocationSet.cpp:77-86，zDriver.cpp:313，zReferenceProcessor.cpp:817 | 删除已无生产者的扫描调度链与状态 |
| 4 | 全表搜索并 CAS | zPageAllocator.inline.hpp:66-73,159-180 | 删除定义、声明、导出 |
| 5 | 正规 empty-page 选择 | zGeneration.cpp:1141-1158 | 已有 ZGC zGeneration.cpp:204-240 形态，保留；生产端刀切 register_empty_page |
| 6 | 正规批量释放消费 | zGeneration.cpp:1119-1125 → zHeap.cpp:509-525 → zPageAllocator.cpp:636-645 | 保留；消费端刀切 free_empty_pages 末尾刷新调用或 allocator 容量释放，测试读真实 GC 两周期后的页所有权和容量 |

ZGC 锚根 /root/cj_build/reference/jdk/src/hotspot/share/gc/z/：zGeneration.cpp:204-240；zHeap.cpp:275-280；zPageAllocator.cpp:2253-2266。无基础设施例外。按常备裁决 5，扫描的上下游清理属于同一修法链。
