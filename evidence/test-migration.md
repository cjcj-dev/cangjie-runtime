# P01 测试迁移登记（进行中）
- colour_state_machine_probe.cpp 与 CMake C4_TABLE configure 臂：所测 EpochColours/ComputeBadMasks 和 action 副本已由 P01 删除；迁入真实产品全局 flip 验证，不能再用 constexpr 模型冒充运行发布。
- gc_unit/test_colour_address.cpp：以 ZGC test_zAddress.cpp:28-435 移植的真实产品谓词/四 flip 组合替换旧 low48 Color/IsLoadGood 模型。仍需补地址往返与精确转红；旧测试名集合差在交付前完整列出。
- 当前尚未运行新测试，不能声明通过。
