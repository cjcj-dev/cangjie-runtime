# 修改前 producer → consumer（基线 8cbac1ea8ef31dbaf5d49d3a1fe67300938ae74c）

| 顺序 | producer | consumer | 修改要求 |
|---|---|---|---|
| 1 | zThreadLocalAllocBuffer.cpp:231 AllocateImpl | zObjectAllocator.cpp:168 alloc | TLAB 改为同一对象分配器；等待编译器 ABI 裁决 |
| 2 | zObjectAllocator.cpp:168 alloc | zObjectAllocator.cpp:134 AllocateSharedPage | 分离 PerAge shared/medium/small/large 函数；medium 锁内强制 non_blocking，锁外才准 stall |
| 3 | zObjectAllocator.cpp:137 Heap::alloc_page | zHeap.cpp:531 alloc_page | flags 完整下传，fast_medium 不丢失 |
| 4 | zHeap.cpp:537 TakeRegion | zPageAllocator.cpp:800 TakeRegion | claim 前分路；flags.non_blocking 决定是否入等待队列 |
| 5 | zPageAllocator.cpp:824 ClaimAllocationLocked | zPageAllocator.cpp:719 ClaimAllocationLocked | 按实际取得尺寸记账，非最大请求尺寸 |
| 6 | zPageAllocator.cpp:724 ClaimPageMemory | zPageAllocator.cpp:236 ClaimPageMemory | fast_medium 只取 mapped cache；命中尺寸回传；不得进入扩容路径 |
| 7 | zPageAllocator.cpp:838 MaterializePageMemory | zPageAllocator.cpp:342 MaterializePageMemory | 消费 PageMemory 实际字节尺寸 |

路径均相对当前工作树 runtime/src/Heap/z；参考 ZGC zObjectAllocator.cpp:66-195，zPageAllocator.cpp:764-785,1546-1547。
切刀选运行时既有消费者 Heap::alloc_page → TakeRegion；先建立产品结果断言后运行切刀，不预宣称闭环。
