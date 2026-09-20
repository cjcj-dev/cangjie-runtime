LANE=sym_cangjie_runtime_720_implement_r5746103729
# #720 构造预留落地后的共享夹具旧注入冲突
351cad8f 已把 reservation 移到 Heap 成员 allocator 构造，显式参数由 HeapManager 调用前计算；gc_unit_main 统一显式128MiB构造 standalone Heap，真实 Runtime 用例不预构造。两构型rc0；ZPageGranule 原样通过 table=4096 expected=4096。
剩余差集出现独立 VM/map 组件被不必要的 Heap 预留占满地址域（正在将这些独立用例标为自建组件、不创建 Heap，不改断言/名单）；另4项 relocation 夹具失败：GcHeapFixture 在 product Heap 自有 allocator 预留之后，仍通过 ZPage::Initialize/Heap::OnHeapCreated 覆盖全局 metadata/reservedSegments，产品真正申请复制页时，返回实际 allocator 的地址被新 synthetic reservedSegments 拒绝，红在 zPageAllocator.cpp:85 IndexOf。
这不只是已准留的页表/forwarding保存恢复，而是旧夹具元数据域注入与产品构造后已预留的 allocator 冲突。建议将 GcHeapFixture 的6粒度 backing 改为从产品 allocator 获得连续页/内存，再按现有 fixture 描述符分配；不再改写全局 ZPage::Initialize/Heap::OnHeapCreated。LargeArrayFixture 同类。需要确认本包可做这一夹具迁移，或给不新增注入面的具体替代。不得按失败改产品 IndexOf 判据，未这样做。
