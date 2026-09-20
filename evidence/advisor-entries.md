接受 004805Z 裁定；本轮以候选 1d1cf0af 为断线基线，并明确非原主线刀。
逐刀入口需求（实读候选）：
1. medium flags 生产点 runtime/src/Heap/z/zObjectAllocator.cpp:213 alloc_object_in_medium_page 内 fastMedium.set_fast_medium；请登记该真实入口函数。消费刀可落既有 Heap::alloc_page 中 manager.TakeRegion(...flags) 对 medium 丢 flags，已有登记。
2. 分段真实入口不是 ZeroAndFill：runtime/src/ObjectModel/MArray.inline.h:123 MArray::NewKnownWidthArray → :141 ZObjArrayAllocator(...).initialize；生产刀将该路由跳过（断开为普通初始化），期望 GC窗口测试 after==before 精确失败；消费刀 runtime/src/Heap/z/zObjArrayAllocator.cpp:29 initialize 内 :144 complete->SetInvisibleObject(false)，改为 true，期望最终 published=0；请登记精确 path:symbol NewKnownWidthArray 与 initialize。
3. stall 等待消费点 runtime/src/Heap/z/zPageAllocator.cpp:648 RegionManager::StallAllocation，:664 ScopedEnterSaferegion→request.Wait，去掉 saferegion 进入，WaiterBlocksInSaferegion 目标 safe=0。请登记 StallAllocation；其 producer TakeRegion :809 接 non_blocking，为完整边界是否也登记 TakeRegion？
4. 请确认“复用同一构建目录”意指每个刀可复用自己绿/红/恢复构建目录？我将使用独立刀目录，遵守两构型 helper 与并行要求，最终以保留 SO 运行相同 ELF，绿=恢复 SO 哈希一致。
继续补测试与实跑；不碰共享入口表。
