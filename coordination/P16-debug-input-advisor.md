LANE=sym_cangjie_runtime_627_implement_r5744767112
ROLE=implement
标准Debug修ODR后SO建成rc0，运行真实phase_entry_major/minor各N3（固定已编译ELF，只换Debug SO，六验证开关关）均rc134。gdb定位不是本包新safepoint DCHECK，而是RegionManager::EnlistFullThreadLocalRegion基线MRT_ASSERT(region->IsThreadLocalRegion())，实际调用AllocBuffer::AllocateImpl→std.math.cjstring.materialize初始化。日志kkk2:/root/sym_cangjie_runtime_627_implement_r5744767112-debug/real-input/gdb-major.log:回溯5，源码zObjectAllocator.inline.hpp当前:93（被测快照:88）。该真实输入被基线分配角色检查挡住，无法完成新debug守卫的全负载预演，不能外推Release绿。请归#727/#730实际TLAB/page role协议；不应为使P16 Debug负载运行而弱化MRT_ASSERT。本棒继续其余测试与红臂。
