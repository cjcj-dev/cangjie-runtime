# 返工生产/消费顺序（修改前，坐标df3e113e61356ccbe6497c226024061cbe6a5c80）
待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。
|面|生产→消费|切刀|
|R1|HeapManager.cpp:25配置容量→zObjectAllocator.cpp:113缓存启发式→zObjectAllocator.hpp:63选择CPU槽→zObjectAllocator.cpp:239共享分配→返回地址/实际页槽断言|alloc_small_object调用把共享槽错接slot0（基线真实入口行）|
|R2|MObject.cpp:34→RegionSpace::TryAllocateOnce zObjectAllocator.cpp:347→AllocPinned zObjectAllocator.inline.hpp:32真实取页→:39重进saferegion→:44 ResetPageSequence→:48发布→返回对象所处page IsAllocating|去掉重采样消费点；若header内联实际进入SO需nm/disassembly证明；GDB在真实获取后插GC，不在产品中新增接线|
|R3|Heap::alloc_page→TakeRegion zPageAllocator.cpp:834 EnqueueLocked→:838 StallAllocation→:653 CaptureWaveBoundary→:655 RequestGC→mark observer窗口第二个真实alloc_page→:658 CompleteWave(boundary)→循环下一波→request.Wait结果|CompleteWave改用GC完成后边界，目标late返回序号不得等于第一波mark序号|

R2/R3窗口裁定：/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_730_implement_r5746939673-20260920T021347Z.md。R3复用现有observer窗口，随#736迁移。R2为GDB注入驱动（非产品路径），N=1确定性构造。
