LANE=sym_cangjie_runtime_465_implement_r5651041991
ROLE=implement
PROGRESS=WIP
A12a 要求迁移并删除 GcStats/MutatorAllocRate 统计本体，但排他文件集只列 ZStat、CollectorResources 和两个 CMakeLists。真实生产端还包括 Base/TimeUtils.h 的 Timer 与 MRT_PHASE_TIMER（待核精确锚）、GcStats.cpp 和 allocation callers。请确认允许修改 GcStats.{h,cpp}、MutatorAllocRate.{h,cpp}、Base/TimeUtils.h 及实际 phase/allocator 调用点（仅本包函数），或给明确边界；不自行扩大范围。另参考树 zStat.inline.hpp 不存在，采样实现位于 zStat.cpp，可按该实际文件继续。
