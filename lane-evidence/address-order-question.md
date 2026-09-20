LANE=sym_cangjie_runtime_720_implement_r5746103729
# #720 新实测：ZAddressOffsetMax 在 allocator.Init 中缩小，构造时点问题
v2 cdd9576e 两构型rc0，testable全套与冻结基线初步候选独红仅 ZPageGranule.MutatorAllocatesThreePageSizes。真实 InitCJRuntime 运行输出：table=2097152，expected=4096，其余三类页大小/页表覆盖断言全部通过，rc1红在test_zPage.cpp:143。
原因：ZInitializer 前移后 platform max=4TiB，Heap 成员 page_table/generation 构造定尺=2097152；随后 Heap::Init 中 allocator.Init -> ZVirtualMemoryManager.cpp:210 将 ZAddressOffsetMax 缩到实际预留8GiB。ZGC zHeap.cpp:62 的 allocator 在构造中完成 reservation，先于 page_table/gen；ZGC zVirtualMemoryManager.cpp:261 同样缩小 max。
若严格构造形态，需要把 RegionManager::Init 的 reservation 提到 Heap::_page_allocator 的构造内，而大量 standalone fixture 第一次 Heap::GetHeap 时没有 Runtime，RegionManager::Init 又读 CangjieRuntime::GetLogParam/GetGCParam（依赖 Runtime）。这已超出只前移 ZInitializer。
请裁决：A 本包继续完成 allocator 构造形态（需要独立初始化参数/无 Runtime 夹具处理）；B 当前按构造瞬间最大地址域定尺、测试比较构造时快照，标注 Cangjie Runtime 参数与 Heap::Init 两阶段基础设施差异（只有明确裁决可用）；或其它方案。未改绝对期望凑绿。其余删除/形态/红臂工作继续。
