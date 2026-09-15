# P01 生产消费顺序（产品修改前）
坐标基于 3140f19160afc759ae540e8cc21cb323905d8cc9。

| 生产入口 | 状态生产 | 消费 | 修改顺序约束 |
|---|---|---|---|
| runtime/src/Heap/z/zGeneration.cpp:62 StartYoungMark | runtime/src/Heap/WCollector/WCollector.h:313 flip_young_mark_start → :273 set_good_masks | runtime/src/Heap/z/zBarrier.cpp:172 编码 healed word | 全局初始化与布局必须先于相位发布、屏障消费 |
| runtime/src/Heap/z/zGeneration.cpp:133 StartOldMark | runtime/src/Heap/WCollector/WCollector.h:325 flip_old_mark_start | runtime/src/Heap/z/zBarrier.cpp:209 store word | Finalizable 跟随 old mark 翻转；编译器须同时更新 |
| runtime/src/Heap/z/zBarrier.cpp:156 make_load_good | runtime/src/Common/ColourEncoding.h:140 MakeStoreGoodSlotWord | runtime/src/Heap/z/zBarrier.cpp:174 ZgcSelfHeal | 地址恢复后由统一 ZAddress::color 编码，结果进入真实槽 |
| runtime/src/ObjectModel/RefField.h:483 derived address store | runtime/src/Common/ColourEncoding.h:140 | runtime/src/Heap/z/zAddress.inline.hpp:80 uncolor_bits | 地址域/HeapBase 与移位表契约必须先固定 |

ZGC 锚：/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAddress.cpp:78（单一全局发布）、:106（初始化）、:143（old mark/Finalizable）；zAddress.inline.hpp:599（offset→HeapBase 地址）、:609（移位解码）、:734（移位编码）。
机械索引见 producer-consumer-index.txt。此表仅记录已读路径，尚未完成全部承重面枚举；不作为运行验证。
