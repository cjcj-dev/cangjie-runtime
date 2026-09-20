LANE=sym_cangjie_runtime_471_implement_r5747758651
四臂差分 e9f1d3677b78 vs b6d62daa：default/filler/managed CAND-ONLY=0；testable CAND-ONLY=5。
新增失败：MarkingStacksProduct.Major{Serial,Parallel,Foreign}EntersFromDoGarbageCollection；ValueRootCurrentization.MajorDriverPairsExportOwnersBeforeHandoff；YoungConc.PauseMarkEndNeverRunsClosure。
真实定位：kkk2:/root/sym_cangjie_runtime_471_implement_r5747758651-green/major-failure-backtrace.log。调用链 old.collect→select_relocation_set→free_empty_pages→Heap::free_empty_pages→RegionManager::free_page→FreeRegionManager::FreeMemory；fixture 的 RegionManager 未初始化 partitions，synthetic 页被真正释放时访问无效 backing。
基线 ZGeneration::free_empty_pages 仅 selector->clear_empty_pages（未真正释放）；我在删除 ExemptFromRegions 的 selected-size freed 账时额外补齐了 ZGC zGeneration.cpp:169-176 的 Heap::free_empty_pages + increase_freed 两行。这两行不是 task 明列要求，但我按完整选集形态补入，现证明扩到共享 fixture 生命周期问题。
任务边界要求共享夹具写后续项、不擅自扩大。请裁定：本条是否撤掉新增 free_empty_pages 两行（保留基线行为，空页释放缺失单独新 issue），还是允许迁移上述 fixture 为真实 allocator 拥有的页？不加跳过、不改断言、不把新红直接标基线。
本条核心 old 调用移位、统一 relocate、统计与 Director 的真实目标用例已通过；生产/消费断线臂正准备。保持 WIP。
