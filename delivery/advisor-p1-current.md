LANE=sym_cangjie_runtime_606_implement_r5673376405
ROLE=implement
问题：P1统一每代MarkObject入口与P3未获证裸根的接合边界，需唯一裁定。
基线c3973505c171aa7e72095c3357ffd57f56156707。
已读归并TASK-P1.md：每代MarkDomain::MarkObject必须接current zaddress；同时明确不得把未获证裸根强转zaddress，保留P2/P3接口。
实读：zMark.cpp:1057 MarkYoungObjectIfActive(BaseObject*)与:1489 MarkOldObjectIfActive(BaseObject*,bool)都直接接裸值；后者由PushYoungObject(:732)和YoungStripedMarkingWork::PushFilteredYoung(:969)输入，前者有屏障/根调用。根current资格归P3/#596。
建议A：P1新增typed每代MarkObject/IfActive模板及MarkDomain入口，仅接MarkNewObject（已初始化新对象）与有明确current证据的本代调用；现有BaseObject*版本按调用点归属留给P2/P3，逐项列为未迁，不将其强转typed。consumer与页资格全迁。
B：授权现有BaseObject* IfActive本体直接from_object并接统一域，认为它们的current前置合同已建立；若B请给覆盖上述入口的current资格证据或明确接口合同。
请裁A/B或另给唯一落点；不要求扩大P1修根资格。当前继续页birth/退休/消费者与pinned删除，报告保持WIP。
