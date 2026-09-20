# #740 producer → consumer 顺序（产品修改前）
坐标基于 b6d62daa8f3557c8a9effbfa4709a4744e315497。

| 顺序 | 我方产品锚 | ZGC 锚（/root/cj_build/reference/jdk/src/hotspot/share/gc/z） | 不变量 |
|---|---|---|---|
| 1 | runtime/src/Heap/z/zBarrier.cpp:292 WriteReference → StoreBarrier :262 | zBarrier.inline.hpp:695; zBarrierSet.inline.hpp store_at | 存新值前取 prev、慢路分流 |
| 2 | runtime/src/Heap/z/zBarrier.cpp:275 → buffer.add :279 | zBarrier.cpp:253-263 | 可缓冲时入配对 p,prev |
| 3 | runtime/src/Heap/z/zStoreBarrierBuffer.inline.hpp:8 | zStoreBarrierBuffer.inline.hpp:37 | 缓冲满先 flush，后入队 |
| 4 | runtime/src/Mutator/Mutator.h:497-504 / Mutator.cpp:194 | zMark.cpp:998-1004 | 先 flush store buffer 后 flush mark stacks |
| 5 | runtime/src/Heap/z/zStoreBarrierBuffer.cpp:154-163 | zStoreBarrierBuffer.cpp:263-280 | make_load_good(prev) → mark_and_remember(p,addr)；本包拟消除手写拆分 |
| 6 | runtime/src/Heap/z/zBarrier.inline.hpp:366-372 → zHeap.cpp:177 → zGeneration.inline.hpp:24 → zMark.inline.hpp:16 | zBarrier.inline.hpp:736-751 → zGeneration.inline.hpp:118-129 → zMark.inline.hpp:48-87 | 根据对象代选 active mark；allocating 页不推栈；remember 根据 slot 所属 old 页 |
| native | runtime/src/Heap/z/zBarrier.cpp:329-339 | zBarrier.cpp:272-278 | native prev 直接标记，不登记 remset |
| phase | runtime/src/Heap/z/zStoreBarrierBuffer.cpp:120-150 | zStoreBarrierBuffer.cpp:201-244 | relocate → remember → mark，旧代 mark 颜色与字段页条件 |

修改必须在消费者重复拆分处；不改变 ZMark::IsAllocating 分路。红臂将分别切现存 WriteReference 调 StoreBarrier、StoreBarrier slow 入队、Flush 消费、remember 页记录。每刀单独产物，测试 ELF 固定；目标状态取产品缓冲、mark stacks、页 remset。
