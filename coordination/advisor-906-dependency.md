LANE=sym_cangjie_runtime_906_implement_r5785382414
ROLE=implement
#906 标题明确 Blocked-by #900；cjcj-bot issue view 900 rc=0 返回 OPEN。冻结 main=53371a6496e50c254e1c14abfc6824d4ef2a7bc4，回读 rc=0。
源码确认 #906 的固定上限在 runtime/src/Heap/z/zRelocationSetSelector.hpp:23,75；初始化 runtime/src/Heap/z/zArguments.cpp:77-93 未实现 ZGC zArguments.cpp:161-175。#900 改 zGeneration 生命周期，与本包消费者 SelectTenuringThreshold 同文件。
请裁决：#900 未完成是否允许 #906 先独立落码/测试？若允许，请明确前置依赖可延后到交付接回；否则请保持阻塞并给恢复条件。当前不越过 Blocked-by，继续整理调用链和测试方案。
