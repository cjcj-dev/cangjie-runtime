LANE=sym_cangjie_runtime_571_implement_r5668109509
ROLE=implement
PROGRESS=WIP
冻结 e26fcb34329464aefefb08873795faae80f51366 回读 rc=0。核锚发现：ReadStaticRef 已直接 LoadBarrier(nullptr, field,...)（zBarrier.cpp:264-266）；LoadBarrier 对 load-bad 已 make_load_good + CAS self-heal（:297-336）；EnumRefFieldRoot 对 mark-bad 已 make_load_good + HealSlot（zMark.cpp:106-163）；markBad 含 loadBad（WCollector.h:281 / zAddress.inline.hpp:16），合法 colored stale 槽应已到 remap。
最新路线要求“分别撤销 remap/入口修正各自转红”“产品基线精确失败”。目前可确认缺口是 plain 非空仍满足 mask-fastpath，但 compiler #1 后这类输入应不再产生；若仅统一 major→ReadStaticRef 或入口加 ValidateCurrentValue，均未找到对合法 colored 根的行为修复。
请求明确：若真实搬迁构造证明合法 colored 基线已满足 current/heal，是否允许以测试覆盖现有接线＋撤销既有 remap/入口 cut 交付，报告证伪必须新增 remap 的前提？还是要求引入 NativeSlot plain 非空编码校验（需先真实输入预演，旧 compiler 静态根已知正常输入会命中，托管验收挂 #585）？不改 LOADFC、不新增 side metadata。继续独立构造真实搬迁测试，不等待答案停工。
