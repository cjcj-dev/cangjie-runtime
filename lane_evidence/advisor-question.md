LANE=sym_cangjie_runtime_844_implement_r5769572061
ROLE=implement
# P-A2 前提核实与 P-A1 前置
冻结 runtime main 回读 779924ea4ca8d91e2f1fd049410f17e419d6eb8b rc=0。
1. 当前已具备 ThreadGCData::storeBadMask、store_bad_mask_offset（runtime/src/Heap/z/zThreadLocalData.hpp:25,49）；ZGlobalsPointers::set_good_masks 发布快照（zAddress.cpp:73），InstallMasks 在 StackWatermarkSet::finish_processing 两分支更新（zStackWatermark.cpp:162,183）。不是缺一个字段。
2. ZGC zStackWatermark.cpp:177-192 在 start_processing_impl 的 process_head 后更新 thread masks，并不是颜色翻面处直接一次写完所有线程。任务不变量2的文字与参考分路点不一致。拟按真实 ZGC start_processing_impl 对齐，而非在 flip 强制遍历写线程；请裁决。
3. /root/cj_build/llvm_rebase/llvm/lib/CodeGen/CJBarrierLowering.cpp:644-658 已有 EnableTaggedPointer && !CangjieJIT 条件下 WriteBarrier::storeFastPath，:525 读全局 g_cjStoreBadMask、:578 直接 store，失败才保留 MCC。所以“所有引用存储均出线”不是当前发射源码的无条件事实；不声称部署 SDK 开启它。将交先于改码的对照表。
4. #843 P-A1 仍 OPEN，REPORT-sym_cangjie_runtime_843_implement_r5769555569.md 当前 WIP 等裁决。P-A2 明确 Blocked-by P-A1；我先完成发射端表与相位链读证，不碰重叠 slow-tier 文件。请给前置可接入坐标或等待/范围裁决。
