# #471 源码接线核对（冻结 b6d62daa8f3557c8a9effbfa4709a4744e315497）

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

## producer → consumer 顺序（改码之前）
|阶段|生产值与消费|源码|
|old collect|调用 concurrent_select_relocation_set 空体，随后 pause_relocate_start|runtime/src/Heap/z/zGeneration.cpp:1188,1279|
|共用 select|page livemap → selector.register_live_page → selector.stats|runtime/src/Heap/z/zGeneration.cpp:1422|
|统计汇总|selector.stats → statHeap.AtSelectRelocationSet → _atMarkEnd.live/garbage|runtime/src/Heap/z/zGeneration.cpp:1445；runtime/src/Heap/z/zStat.cpp:809|
|Director 采样|old.StatHeap()->Stats → old_stats.stat_heap.liveAtMarkEnd|runtime/src/Heap/z/zDirector.cpp:628|
|Director 消费|old_used - min(live, old_used) → extra young gc time|runtime/src/Heap/z/zDirector.cpp:350|
|搬迁 worker|old relocation_set → ForwardTask → parallel iterator → ForwardClaimedPage|runtime/src/Heap/z/zRelocate.cpp:1261；runtime/src/Heap/z/zPageAllocator.inline.hpp:264|

统计应随 ZGC select_relocation_set 在 install / flip / forwarding table 后汇总；old select 必须在 pause_relocate_start 前。
红臂生产端计划断 collect 中基线已有 concurrent_select_relocation_set 调用；消费端计划断 Director 中基线已有 StatHeap()->Stats 采样。验收须读真实运行采样值而非手工拼接。

## 已证伪的子前提
- ForwardFromRegions<Old> 已消费 old.relocation_set，不是另选一份 page list；共用消费者删除范围已问 advisor。
- zGeneration.cpp:1435-1439 是 selector 年龄统计，非逐页 livemap 重加；ZGC :720-738 同形。
- zHeuristics.cpp 已有 medium + NUMA；Director 自行构造 headroom 才缺 medium。
- ZCollectionIntervalOnly 并非全缺：minor allocation 和 major proactive 已有；缺 minor high usage 和 major warmup（ZGC zDirector.cpp:366,402）。

## 测试盘点
现有 runtime/tests/gc_unit/test_relocation_set_selector.cpp 覆盖 selector 算法。现有 test_gc_director.cpp 主要覆盖统计组件，不能直接替代 old collect → Director 运行闭环。参考 JDK test/hotspot/gtest/gc/z 文件索引未发现 RelocationSetSelector 专用 gtest；需保留检索 rc 并核完整目录。
