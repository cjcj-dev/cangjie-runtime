LANE=sym_cangjie_runtime_503_implement_r5656150642
ROLE=implement
PROGRESS=WIP
D10 retained 删除消费端对照：ZGC zRelocate.cpp:1257-1280 ZRelocateAddRemsetForFlipPromoted::work 对 relocation_set->flip_promoted_pages 的原 young ZPage 直接 object_iterate；zPage.cpp:64 clone_for_promotion 创建独立 old ZPage，仅复制类型/地址布局/top，原 young livemap 保留。我方 RegionInfo::PromoteYoungRegion（冻结 zPage.inline.hpp:1747附近，当前工作树:1470）在同一 RegionInfo 上把 liveInfo 清空并 InitializeLiveInfo；AddFlipPromotedPage 仅存 RegionInfo*，RememberFlipPromotedPages 原靠 retained 位图副本回读。删除副本并换 CollectLiveObjectStarts 仍会在 Promote 后丢掉普通 livemap，因此不能交这份部分改动。
请裁定本轮实现边界：建议将 flip_promoted_pages 元素直接持有晋升前的普通页视图（原 LiveInfo 而非克隆位图、原 top/start），并由生命周期所有权保留至 worker join 后释放，对应 ZGC 原 young ZPage 的拥有关系；或应在本轮完整引入 RegionInfo clone_for_promotion（涉及页表映射和 UnitInfo array 布局，超出 retained 删除接口）？不希望另发明一个 renamed retained snapshot，故先问。根过滤清理及 VerifyLive 下沉继续执行；报告保持 WIP。
