LANE=sym_cangjie_runtime_596_implement_r5673746875
ROLE=implement
承接B裁定，真实测量已证伪C1-C4 current合同：复用test_native_root_current.cpp的真实CompactRegion fixture，创建注册runtime mutator分别持C1栈对象字段/C2NativeFrameRoot/C3invisible/C4headerless的旧色from；通过产品TraceHeap→线程握手→GcPhaseEnum→标记闭包。
kkk2:/root/sym_cangjie_runtime_596_implement_r5673746875-base/evidence/a2-baseline-unit.log:3839 C1 current_marked=0 stale_marked=1 healed=0；:3863 C2同；:3887 C3同；C4同（完整日志后段）。四项到达A2_THREAD_ROOT_TARGET后红在native_root_marker_current，编译/加载成功。基线其余完整单元0；新增完整测试rc1。
产品根槽旧色在GcPhaseEnum C1/C4被PlainRootObject丢弃，C2/C3被StripRootObjectColour丢弃；实际没有在本次mark前remap。C1甚至原槽仍是旧色。
拟最小修复：Mutator.cpp的PushHeapRoot由plain value改接真实RootSlot，保留observed word，带色历史输入经collector.make_load_good解析后发布current，再同槽写plain；C1/C2/C3/C4统一用此入口；plain输入仍按eager preforward合同，不猜page owner。无ReadStaticRef、不改CHECK、不改StackWatermark基础设施、#577路由不改。
但上轮B把「uncolored同槽barrier」归#498，本动作可能撞此边界。请明确：A 允许本包修改既有PushHeapRoot形成上述eager输入修复（形态例外仍保留）；B 本包只交四项失败证据并退回归并/方向，由#498先修输入合同（请给next_stage）；C 其他。不把这次红写成修复验收。
