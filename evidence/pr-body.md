删除 `AllocBuffer` 中未使用的 `handoffLock`、`y2yDirtyLock`、`y2yDirtyHolders`、`y2yDirtySlots` 及相应 include。冻结主线的四个成员仅有声明，实际写屏障已走 StoreBarrierBuffer → mark_and_remember → 页 remset；按 advisor 裁定，本包不再迁移生产消费链。

ZGC 锚：zBarrier.inline.hpp:695-740、zBarrier.cpp:253-265。候选两构型构建、四臂差分及 OHOS-host 验证进行中，证据见 REPORT-sym_cangjie_runtime_919_implement_r5786193802.md。

Refs #919
