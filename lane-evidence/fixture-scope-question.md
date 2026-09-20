LANE=sym_cangjie_runtime_720_implement_r5746103729
# #720 夹具边界裁决请求
冻结基线 8cbac1ea，核心改动进行中：FoundOld 直接成员构造、remembered 移入 young、构造注入、ZInitializer 前移、删 bind/test allocator 路由。
现有 GcHeapFixture/ZTestRegionHeap 不只是 bind：ZFixtureRememberedScope 会 move Heap 的 page table/forwarding/remembered；GcHeapFixture 消费方和 RegionManager::ReturnPageMemory/RetirePage 等仍通过 Heap::page/Heap::remembered 取全局对象。任务要求 gtest 自建 allocator/page，不注入 Heap。若彻底消除所有 fixture 全局表注入，需要迁移大量共享夹具消费方，超过 issue 列明的生命周期范围。
建议：本包删除 allocator 注入，allocator 测试直接调用自己 manager；共享页表夹具暂保留既有 page-table/forwarding 保存恢复，仅将 remembered 改为真正 generation 构造生命周期（不再 bind）。是否允许共享夹具保留页表保存恢复，并以 ZGeneration::young() 访问构造得到的 fixture generation？或者本包必须同时移除所有全局页表夹具？请明确范围；我先完成独立核心与依赖调用迁移。
