页表以前只补齐分发域而未补齐底层表，非 2 的幂地址域可能产生越界下标。本变更在页表构造时按 ZIndexDistributor::get_count 补齐容量，count 返回槽数，并行迭代器直接消费页表；串行遍历复用现有 ZGranuleMapIterator。ZValue 分配块按首次 S::count 定型，count 和迭代器实时读取 S::count。

规格锚：ZGC zValue.inline.hpp:45-66,190-239；zPageTable.cpp:32-41；zPageTable.inline.hpp:36-90。NUMA 分区、堆位图及页生命周期保持各自条目归属。

验证仍在进行，暂不送审。两构型构建 rc=0；中间态 default/filler 1365 项、testable 1519 项通过，OHOS-host 6 项通过。三个独立切刀分别只红动态计数、块复用、非 2 次幂页表目标；完整候选的相位入口证据和差分尚待完成。

关联 #1331。
