LANE=sym_cangjie_runtime_608_implement_r5676392826
ROLE=implement
PROGRESS=WIP
P01 旗标分代接入核到阶段边界：runtime/src/Heap/z/zRelocationSetSelector.cpp:323 RegionManager::ExemptFromRegions 唯一 SelectRelocationSet(descs) 产品消费者（:458）；年轻代 PrepareYoungGarbageCandidates(:186) 在 mark 前按所有权/页角色选候选，不调用该 selector。已新增 z_globals.hpp 的 old=5/young=25 单表及 zGeneration.cpp 的 fragmentation_limit 分路，old 消费者已接 old5；不能声称 young25 已被真实年轻代选择路径消费。
ZGC 对应 zGeneration.cpp:197-207 在 select_relocation_set 中构造带本代阈值的 selector。若本包要让当前年轻代真实消费25，需要把选择器接入 mark 后的 young select_relocation_set，相位/选择器分解属 P11/P14 的主范围；不能把阈值直接塞进 mark 前 PrepareYoungGarbageCandidates（活性输入时序不同）。
请裁本包边界：将 young selector 消费者接入明确交 P11（本包交旗标/分路与 old 接线，并登记无当前年轻代 selector 消费者），还是授权 P01 同改年轻代相位选择接线并给与在飞 P11/P14 的范围约定？其余 ABI/三仓构建继续。
