# #814 producer → consumer（冻结 d49c1ffd4de13032f5bc8496a298e93b0ba87489）

|顺序|产品锚|值身份|
|---|---|---|
|1|runtime/src/Heap/z/zMark.cpp:74-79|MarkBarrierOnOopField 修复 export slot，IncomingNew 入 oldExportOwners|
|2|runtime/src/Heap/z/zGeneration.cpp:997|真实 mark_end 调 ProcessExportRoots|
|3|runtime/src/Heap/z/zCrossVM.cpp:298|ResolveCurrentValueRoot 解出当前 exportObj|
|4|runtime/src/Heap/z/zCrossVM.cpp:306|隐式 ValueRoot(exportObj) 丢失当前身份；默认 OverwritePrevious|
|5|runtime/src/Heap/z/zCrossVM.cpp:320|foreign 对象同样隐式 push_back 丢失当前身份|
|6|runtime/src/Heap/z/zGeneration.cpp:723|non-strong phase 调 FindUselessExternObjects|
|7|runtime/src/Heap/z/zCrossVM.cpp:288,392|CurrentizeValueRootMap 消费 key.Stage()，错误进入 forwarding|

修改必须在步骤4/5发布前保存 IncomingNew；map 查找同样显式使用当前身份，避免隐式默认构造。
ZGC /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zUncoloredRoot.inline.hpp:62-69：颜色 load-good 时保持地址，不以 forwarding 存在为旧身份依据。
本包保留既有默认构造的 old-value 合同，只修正真实 producer 发布身份。
