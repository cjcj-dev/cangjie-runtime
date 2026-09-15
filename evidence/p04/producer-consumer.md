# 修改前 producer → consumer 顺序表
坐标基于候选8791a8ac1b7d7023884ca3a7861b0a031872885b。机械检索原文在producer-consumer-search.txt。

| 入口 | producer | consumer | 本轮迁移落点 |
|---|---|---|---|
| Allocator初始化 / zPageAllocator.cpp:1289 | ZVirtualMemoryReserver::reserve_contiguous 注册地址 / zVirtualMemoryManager.cpp:129 | RegionManager::Initialize / zPageAllocator.cpp:579；页段表 / zPage.inline.hpp:824 | 保留P01域与实际段发布；P04管理器供页几何，P02标记与P06退役不改回旧版 |
| RegionManager::ClaimAllocationLocked / zPageAllocator.cpp:737 | ClaimPageMemory / zPageAllocator.cpp:253；mapped cache返回 partialMappings | stash_segments → free_and_claim_virtual_from_low_exact_or_many → restore_segments / zPageAllocator.cpp:321-340 | ZArray数组在生产与消费两端迁P06 API，物理stash改ZArraySlice后消费者传最终slice |
| 页面提交路径 / zPageAllocator.cpp:350 | claim_physical / :375；commit_physical / :378 | map_virtual / :388；后续页初始化 / zPage.inline.hpp | 不改六函数分解，数组消费改正式容器 |
| Uncommitter::RunCycle / zUncommitter.cpp:251 | RemoveForUncommit / :193 生成flushedVmems | unmap→uncommit→free physical→free virtual / :205-208；UncommitFlushed / :215 | flush数组与容量记账保持产品结果进入目标断言；保留当前线程外壳，P07拥有未来ZThread |
| Registry insert消费者 | free_virtual / zPageAllocator.cpp:112 | ZRangeRegistry::move_into / zRangeRegistry.inline.hpp:23 | 通用ZList用P06，节点及合并算法归P04；header-only断本体 |

原四刀：range prev合并、stash排序、UncommitFlushed容量递减、mapped cache小页最低地址分路。历史证据与最终候选必须分列；新候选重验后才声明闭环。
