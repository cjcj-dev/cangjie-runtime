# 承重顺序（91653ce85，产品码未变于 f1b274e4c）
| 顺序 | 产品锚 | 结果进入断言 |
|---|---|---|
| 1 | runtime/src/Heap/z/zGeneration.cpp:963 ZGenerationOld::mark_start | 真实颜色、序列与 mark 栈初始化 |
| 2 producer | runtime/src/Heap/z/zGeneration.cpp:1062 ZMark::DoEnumeration(workStack, foreignStack) | 从根生产工作 |
| 3 consumer | runtime/src/Heap/z/zGeneration.cpp:1068 Mark().MarkFollow(false) | 消费工作，数组子对象强存活位与页计数 |
| 4 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:361 ReadArrayMarkState | 产品页状态进入 markedChildren / objects / bytes |
| 5 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:399 GC_EXPECT_EQ(markedChildren, expectedChildren) | 子对象闭包目标断言；切 producer/consumer 各一臂，保留 young 用例阳性对照 |

ZGC 顺序锚 /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zGeneration.cpp:1086-1092。
