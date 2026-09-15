待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
# P1 接口及过渡范围（WIP）

接口：GenerationCycle::MarkObjectIfActive<resurrect,gcThread,follow,finalizable>(zaddress) → MarkObject → MarkDomain::MarkObject。
参数分别对应 ZMark resurrect / GCThread或AnyThread / Follow或DontFollow / Finalizable或Strong。输入必须已有 current 资格；禁止用入口早退替代解析与资格检查。页 mark 原语 true=newly-marked；旧 collector bool 返回约定在其适配接口保留。

advisor A：/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_606_implement_r5673376405-20260915T014739Z.md。
现有裸指针 MarkYoungObjectIfActive / MarkOldObjectIfActive 及 PushYoungObject / PushFilteredYoung 暂留，待 P2 字段屏障或 P3/#596 根历史色资格接线，不认定这些入口已形态一致。P1已接初始化后的 MarkNewObject。
