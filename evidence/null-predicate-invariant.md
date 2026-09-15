LANE=sym_cangjie_runtime_608_implement_r5676392826
ROLE=implement
PROGRESS=WIP
任务不变量1写任一 zpointer: is_load_good(p) ⇔ (p & LoadBadMask)==0，但 ZGC zAddress.inline.hpp:631-633 明确 !is_load_bad(ptr) && !is_null(ptr)，raw null 的 is_load_good=false 而 bad-mask=0。对应 gtest test_zAddress.cpp:201-209（本包已移植 IsChecks 实跑 rc=0）也明确 raw null 只满足 good_or_null。
请确认将式修为 is_load_good(p) ⇔ p!=raw_null && (p&LoadBadMask)==0，或限定 p 非 raw null；is_load_good_or_null 保持单独 bad-mask==0。产品已按 ZGC 原函数实现，不改成 raw null load-good。其它 ABI/三臂工作继续。
