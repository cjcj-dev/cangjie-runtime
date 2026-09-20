LANE=sym_cangjie_runtime_627_implement_r5747848521
ROLE=implement
PROGRESS=WIP
A2 已完成：产品生产/消费切刀重编 SO，三族 N3 精确目标红，正常/恢复绿。A1十项已无条件编入 publication ELF（default/testable 都不再靠已删receipt宏）；七项通过。三个 oldPending 夹具已修页表发布、原生根vector扩容后地址失效、只调ZMark::Start未切Mark相位（改为真实old.mark_start）。现在真实driver已产生 forwarding expected=40000c00000，但原生槽仍 before=40000c00010，目标红。
证据 kkk2:/root/sym_cangjie_runtime_627_implement_r5747848521/raw-v5/RawRemapYoungProduct.MajorKeepsOldPendingThenRelocatesRawRoot.log（rc1），另外 DerivedRoot/Fallback同分路。
源码两侧：我方 zGeneration.cpp:1066-1069 young remap→old relocate-start→concurrent_relocate；:1159/1165 与 zRelocate.cpp:229 无最终 eager raw scan。ZGC zGeneration.cpp:1015-1071/1379-1404 同样 old relocate 后靠下一次根处理；zUncoloredRoot.inline.hpp:35-64 在根处理时原槽 make_load_good+写回。已裁P16不改pause_relocate_start/Preforward（#727），也不恢复旧eager路径。
拟按(ii) ZGC语义更正三个测试的观察时点：driver后先断言旧槽保持before、转发表目标存在且不同；再经现有真实 Mutator::GcPhaseEnum(false) 根处理入口（无手工喂目标）后断言原slot/base/derived等于转发表winner，原有null/nonheap保留。测试改名明确UntilNextRootScan并记T类与ZGC锚；对driver和root-scan产品接线做断线证据。请裁决此观察时点修正，还是三项归其它包且本轮只恢复执行并如实记录基线红。未改期望，先做独立最终输入/矩阵整理，保持WIP。
