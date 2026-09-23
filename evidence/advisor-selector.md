LANE=sym_cangjie_runtime_959_implement_r5790205235
ROLE=implement
Q15 验收构造前提需裁定：正常 small/medium 页均为二次幂（zGlobals.hpp:30、zHeuristics.cpp:67、zPageAllocator.cpp:247、zObjectAllocator.cpp:207）；合法页上除法与移位数值相同，恢复除法+钳制无法让合法页排序断言变红。且生产 select_relocation_set 使用 constexpr ZFragmentationLimit=5.0 / ZYoungCompactionLimit=25.0（z_globals.hpp:8-9、zGeneration.cpp:162-174），pre_filter 必然拦掉高分区，无法从产品入口得到含全部 2048 分区的选择集。
拟保留全部 2048 分区输入，经 old.select_relocation_set 验证过滤后所有合格页的完整顺序（阈值按产品 ZGC 公式，非法输入另列 debug 前提断言）；排序刀用产品 semi_sort 调用断线与 partition 偏移扰动分别证明消费者/生产者因果敏感，而不将合法域的除法恢复称为可转红。是否批准此验收构造修正？产品形态修改已完成且 default/testable 构建 rc=0，未改阈值，未加钩子，报告维持 WIP。
