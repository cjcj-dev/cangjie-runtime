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

## 补注裁决后完整形态

已读 /root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_814_implement_r5764144763-20260921T165356Z.md 和 sym_task 追加的0922 00:5x裁决。早期 producer-only 候选不是最终实现。

ValueRoot 只保存 object/color；删除 Stage/generation 派生身份。每个 producer 当前值构造时保存 load-good 色；consumer ResolveCurrentValueRoot 先 is_load_good(saved color)，坏色按 remap_generation 路由到该代 relocate_or_remap_object（其 get 空回原地址），好色 ValidateCurrentValue。Currentize 重建时刷新地址及色。

ZGC 对应：zUncoloredRoot.inline.hpp:35-69（保存色判定与修复），zGeneration.inline.hpp:131-140（本代 forwarding 表为空保留地址），zRelocate.cpp:382-415（from-key消费）。

诊断：旧 in_current_relocation_set 实为 lookupTo!=0，改 forwarding_lookup_hit；lookup_state 改 not_attempted/hit/miss，未改判断或 LOADFC。

删除清单：ValueRoot::Stage、ValueRoot.stage、ValueRoot.generation、以 forwarding_for_page 为 ResolveCurrentValueRoot 分路。
