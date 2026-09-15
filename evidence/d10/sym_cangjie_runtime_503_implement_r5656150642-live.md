LANE=sym_cangjie_runtime_503_implement_r5656150642
ROLE=implement
PROGRESS=WIP
D10 追加项前提部分被实读证伪：冻结 78fc9ce028de705b3ea705b7069759c1036a2796 的 runtime/src/Heap/z/zForwarding.cpp:276-304 已在 verify() 枚举转发表累加目标 GetAllocSize，然后比较 sources.size()==bitmap->GetLiveObjects() 与 bytes==bitmap->GetLiveBytes()。参考 ZGC zForwarding.cpp:369-406 通过 zPage.cpp:196 verify_live 做同一精确比较。计划把已有比较下沉为 RegionInfo::VerifyLive 并从 ZForwarding::verify 调用，补齐 ZGC 页级函数形态，而不是新增第二条校验链。请确认；该项待答复，先做根启发式和 retained 副本删除。
另：冻结 zPage.hpp:1052 已明确 Only FORWARDING_FACE_RESET_BIT is used，低位计数此前已删。本轮删除剩余字段/死标志方法及 retained 载体全链，报告逐项区分已有删除与本轮删除。
