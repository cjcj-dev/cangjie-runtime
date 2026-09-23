待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标基于 53371a6496e50c254e1c14abfc6824d4ef2a7bc4。

| producer | consumer | 必须的顺序 / ZGC 锚 |
|---|---|---|
| FreeRegionManager::MaterializePageMemory zPageAllocator.cpp:375 | ZPage::InitRegion zPageAllocator.cpp:431 | 缓存命中只创建页描述，不写 payload；zPageAllocator.cpp:1489-1492 |
| ZCollectedHeap::allocate_new_tlab zThreadLocalAllocBuffer.cpp:156 | FillTLAB zThreadLocalAllocBuffer.cpp:159 | refill 得到 actualSize 后清零，再发布边界；gc/shared/memAllocator.cpp:312-324 |
| HeapManager::Allocate MObject.cpp:14,38 | SetClassInfo MObject.cpp:16,40 | 对象 payload 清零在类型发布前；gc/shared/memAllocator.cpp:366-397 |
| HeapManager::Allocate MArray.inline.h:163 | SetClassInfo MArray.inline.h:169 | 普通数组清零在 header 发布前；分段数组仍进 ZObjArrayAllocator；gc/shared/memAllocator.cpp:400-411，gc/z/zObjArrayAllocator.cpp:92-112 |
| ZObjectAllocator::alloc zRelocate.cpp:764 | relocation copy | 复制分配器不承担清零；ZGC zObjectAllocator.cpp / zRelocate.cpp |

删除清单：页层 clearPayload 参数、ZPageAllocation::clear/ClearsPayload、ClearReleasedMemory、ClearPageMemory。页层无 ZGC 对应清零分支。
