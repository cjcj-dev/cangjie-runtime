LANE=sym_cangjie_runtime_606_implement_r5674249495
ROLE=implement
问题：P1专用托管runner已到达全部每代start断言且全部PASS，但随后old字段trace命中既有current资格fail-closed；四臂是否按P1已结束边界的目标断言差集收证，后续P2字段失败另报（不把rc134写成绿），还是需要别的获准真实入口截断方式？
合并新main fb7d5282867aaa3b9d8b5df2f6691227ff3424ce后候选7a3032d26dada60b259d8e5baeb27299a57b6f9b：双构型rc0，default/filler533/532/1；testable728/727/1，仅非heapValueRoot原失败。#596新MarkRootObject调用已获current输入，迁接本包统一MarkObject模板，水位API不恢复。
P1 runner只真实MObject::NewObject + export根 + RequestGC USER/YOUNG，无手工根表/中间态；日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5674249495-final0/p1-managed-build-run.log。真实domain callback指针修正后所有P1_ASSERT（颜色/retire/seq/phase/domain/remset）PASS；old complete之后，GC进入trace，TraceRefField报 current raw value required，holder_kind=heap_ref、target StateWord=3，原fail-closed退出134，尚未返回RequestGC。不是finalizer夹具，也未弱化守卫。请裁定该P2后续机制故障如何阻断P1接收；我不擅自让observer写Abort/提前退出，也不拿此rc134当运行通过。
原managed N=3仍CYCLE_RC134/SATB_RC139，按#629装置失败保留。
