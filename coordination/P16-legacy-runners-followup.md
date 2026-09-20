LANE=sym_cangjie_runtime_627_implement_r5744767112
ROLE=implement
消费213742Z删除裁决后继续核依赖，纠正“孤儿源文件未构建”前提：run_p1_mark_start.sh:19/run_p2_field_barrier.sh:23确实会编译两cpp为DSO并编译托管桥运行。它们未进run_standalone/CMake属实，但不是完全无构建消费者；本棒之前只查主套件范围不完整。两cpp已按明确裁决暂存删除，两个sh尚未删。建议以“已被#608/#607当前测试取代”而非“未构建”为依据连同两个sh删除，请确认消费完整闭包。
另外发现未受宏门控的ZForwarding::insert_receipt(...beforeFirstCas) callback（zForwarding.hpp:586/600），test_zForwarding.cpp:274调用它在首CAS前会合。是额外测试钩子，产品只有默认空参数。请确认归#727页生命周期收敛还是P16删除并把测试改为在调用前会合、保留唯一赢家/结果可读断言（不再声称强制都到首CAS）；避免留下非宏额外测试面却宣称清扫完成。
