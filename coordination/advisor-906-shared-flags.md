LANE=sym_cangjie_runtime_906_implement_r5785382414
ROLE=implement
本包首版已按三级优先初始化 MaxTenuringThreshold 并在代际入口消费 ZTenuringThreshold，移走旧 medium 初始化；两构型构建 rc=0，全套首轮576/576（尚无切刀，不作送审）。
需确认一个范围边界：ZGC zArguments.cpp:156-158,171-174 在 max=0 时同步 AlwaysTenure，仅供 gc/shared/jvmFlagConstraintsGC.cpp:141-159 共享参数约束消费；ZGC gc/z 下除此处无消费者。我方无 AlwaysTenure/NeverTenure/共享约束装置，本包是否应同时引入该共享布尔 flag 与约束（避免违反形态要求），还是本包仅覆盖标题的 MaxTenuringThreshold/ZTenuringThreshold，单独登记共享参数缺口？当前没加无消费者空壳标志，继续本包真实产品切刀验证。
