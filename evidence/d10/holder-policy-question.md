LANE=sym_cangjie_runtime_503_implement_r5656150642
ROLE=implement
PROGRESS=WIP
D10 最终枚举发现 RememberedHolderPolicy.h::KeepRememberedHolder(bool retainedSnapshotSaysLive,bool isCurrentMinorRoot) 仍在，但实际消费已是 zMark.cpp::ScrubMinorFreeTarget 调 SlotHeldByLiveObject(&field)（当前普通页 liveness，非 retained bitmap）。旧快照描述与 OR 当前根放行没有 ZGC 对应。这是 D10 retained 清理范围还是另一包的遗留故障分类器？建议本轮一起删除 KeepRememberedHolder 和专属测试，将 ScrubMinorFreeTarget 调用处仅使用 SlotHeldByLiveObject，去掉其 currentMinorRoot 覆盖输入；其余 ScrubMinorFreeTarget 的 free/garbage→FailClosedLoad 属既有屏障处理，保持。不能只把 retained 名改掉假称删除，故先请求范围确认。
