LANE=sym_cangjie_runtime_720_implement_r5746103729
# #720 名单夹具发现基线旧失败，拟移交 #730
v2 已让 ZValue.shared_small_page、MappedCache、PageRetirement、AllocationStall 及 SharedSmallPage 其余项通过（testable）。SharedSmallPage.AgeRefillAndRetirement 基线也失败：用 alloc(2MiB-32) 期望仍填原 small 页，违反 ZGC zObjectAllocator.cpp:183-193 按对象大小分路（小对象上限远小于2MiB）。红在 test_shared_small_page.cpp:157 地址相等断言。
上条裁决要求“只改夹具初始化方式”，故未改这一旧期望。可选修正是按 min(ZObjectSizeLimitSmall, remaining) 多次分配填满 small 页，每次保留地址连续断言，不改产品。请裁决本包一并修测试输入还是留 #730 处置并作为基线旧失败记录；避免撞车。
