P01 三仓低位元数据已经实施；default/filler 520/520，testable 722项16失败暴露额外根载体边界。
1. ReadStaticRef / MarkYoungGoodBarrier 保留旧“readonly ELF literal NativeSlot 可以装裸非堆地址”路径；低位模型 NativeSlot::GetTargetObject 现在 SHR，裸 literal 无法正确解码。test_native_root_current.cpp:283 ReadOnlyNonHeapBoundary 构造 mmap readonly NativeSlot 装裸 literal；产品 zBarrier.cpp:253-258 自称支持。是否按 P01 将这类原始只读 literal 从 NativeSlot 注册/读取切到单独 plain root 域（需要 root registry 边界，#607/P10所有），或按本包删除旧机制测试并登记首次生产者迁移？不能用猜测添加“看起来像裸地址就不移位”兼容分支。
2. test_native_root_current.cpp:147 在 RootSlot 写 StoreGoodPointer(from)，C1..C4随后跑旧 mark期望历史颜色解码。P01已明确RootSlot普通地址，这四例旧形态应删除/替换为plain旧地址+已有握手 remap，是否由P10拥有？当前产品注释 zBarrier.cpp:271说eager handshake makes current。本棒会保留真实RemapYoungRoots产品测试；它发现WCollector::HealRootWriteback还经RefField(target)写色，此处已修为HealRoot普通地址，正在构建。
3. NativeRootCurrent.MajorSeed目标marker/heal已绿，但第二次TraceHeap仍重新发布已标记根（test:241期望不发布），似为P2根mark节流所有，不能擅改有效断言。请确认归属。
请给最小跨包边界裁决；本棒保持WIP继续新SDK托管验证和明确P01消费者修复。
