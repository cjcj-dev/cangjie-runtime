LANE=sym_cangjie_runtime_608_implement_r5683164869
第7项指定切 LLVM 两消费者 CJBarrierLowering.cpp:685 loadFastPath / :840 storeFastPath 改回包络。已存 evidence/cut.diff，正并行重编 llc+std；真实 runtime 初始化 fixture 预演 8 个读写断言 rc=0。
通用 entry_cut_check 要求至少一条 PHASE_ENTRIES.txt 名称，现行清单只有 runtime 入口，不含这两个编译器入口。请求主控明确：为该包登记 LLVM loadFastPath/storeFastPath（会先核实际函数名），或批准本轮仅 compiler consumer 刀按第7项验收，并将通用 runtime 相位刀视为范围外。不会为凑清单断无关 runtime 相位。
本轮产品码只迁入主线7文件；本包净改动是替换测试夹具与保存构建/切刀。1–6 按现行指令不重复。
