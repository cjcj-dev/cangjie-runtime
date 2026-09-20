LANE=sym_cangjie_runtime_627_implement_r5744767112
ROLE=implement
已新增真实入口测试ZVerify.RuntimeRejectsUnallocatedRootBeforeMark：child InitCJRuntime→StrongRootStorage真实注册坏根→Heap::RequestGC(USER,false)→VM_ZOperation::pause→BeforeZOperation。tests5 default447项已实跑，ZVerify全项通过（总12失败属其它包，尚待差分）。
两刀均基线已有行：
1. phase-consumer.diff删除zGeneration.cpp VM_ZOperation::pause内ZVerify::BeforeZOperation();（只这一处，不碰Preforward）。
2. root-producer.diff删除zRootsIterator.cpp RootsIteratorStrongColored::Apply内strong.apply(&copy);（保留静态根；只切真实strong-storage来源）。
已跑entry_cut_check：两刀产品文件/基线逐字匹配/不是候选新增行均✅，仅phase入口表未含这两个根扫描/VM操作入口，rc1。证据本树coordination/cuts/{phase-consumer,root-producer}-check.json与*.diff。
请核准在PHASE_ENTRIES登记精确作用域runtime/src/Heap/z/zGeneration.cpp:pause和runtime/src/Heap/z/zRootsIterator.cpp:RootsIteratorStrongColored::Apply（若解析器只取末名则Apply）。前者是实际stw VM相位包装，后者是其直接根枚举producer；新测试不直接调用任何验证/枚举helper。红臂使用隔离副本SO与同一测试ELF，已准备并继续运行，未把形式rc1冒充通过。
