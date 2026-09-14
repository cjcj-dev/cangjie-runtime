# producer → consumer，坐标333d3216762495d49d34594390c3afa05378010f

| 顺序 | 产品锚 | 必须观测的状态/值 |
|---|---|---|
| 1 | runtime/src/Heap/z/zRelocate.cpp:2065 CompactRegion → PromoteYoungRegion | 页从年轻变老，carrier仍为Young；不手写晋升结果 |
| 2 | runtime/src/Heap/z/zRelocate.cpp:2082 CopyObject；:2087 InsertMapping | 非零位移，同页from→to映射及payload保持 |
| 3 | runtime/src/Heap/z/zRelocate.cpp:2097 ZeroAndFill；:2107 RehomeCompactedInPlaceRegion | 页面真实重新挂接，from位置为产品生成的filler（专门选择等大小死前缀） |
| 4 | runtime/src/CompilerCalls.cpp:969 Acquire → PinArray；:937 虚调用PinRawPointerObject | 真实SO虚表，禁止局部WCollector虚表替代产品 |
| 5 | runtime/src/Heap/WCollector/WCollector.h:453-458 | carrier源代相位选择，再消费映射 |
| 6 | runtime/src/Heap/WCollector/WCollector.h:464-465 | to页pin计数、返回to对象 |
| 7 | runtime/src/CompilerCalls.cpp:991、:1024 | 返回to payload；Release按rawPtr所在页减计数 |

计划保留已有4项；新增真实Compact→Acquire/Release在PREFORWARD/FORWARD；反向源代IDLE/另一代FORWARD用产品filler输入，断言不解析且返回from payload；普通无carrier路径单独有界输入。所有输出先捕获、清理后断言，使定向刀不影响后续用例。
ZGC锚：/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRelocate.cpp:862-896；/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zGeneration.inline.hpp:131-140。
