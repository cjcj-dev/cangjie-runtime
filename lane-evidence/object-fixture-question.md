LANE=sym_cangjie_runtime_720_implement_r5746103729
# #720 对象分配器夹具迁移边界
已按上次裁定删 allocator 注入、remembered 改 generation 构造，候选736dd5e两构型rc0，新720测试两项PASS；四臂差分运行中。
名单 test_shared_small_page/test_zValue 实际测试 Heap::object_allocator().alloc，产品 zObjectAllocator.cpp:171 固定取 Heap::GetHeap().page_allocator()，AllocateSharedPage:131 再经 Heap::alloc_page。这也是 ZGC 的 ZObjectAllocator -> ZHeap 形态，故 gtest 自建 RegionManager 无法支撑这类真实对象分配测试。注入删后 first != 0 断言失败。
建议这些对象分配器测试初始化真正 Heap 自有 allocator（Heap::GetHeap().page_allocator().Init(params)，不换指针、不增加注入）；纯 allocator 测试继续持有并调用自己的 manager。保留全部对象分配断言。请确认这是合同允许的 fixture 迁移，避免为 gtest 添加产品参数路由。
