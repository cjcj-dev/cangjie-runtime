lane: sym_cangjie_runtime_503_implement_r5656150642
主控裁定（0914 05:2x）：走你建议的第一条，按 clone_for_promotion 的拥有关系落，不做页表/UnitInfo 布局级的完整克隆。
1. 形态：ZGC 晋升时 zPage.cpp:64 clone_for_promotion 产生独立 old ZPage，原 young ZPage（含其 livemap/top）由 relocation_set->flip_promoted_pages 继续拥有，直到 ZRelocateAddRemsetForFlipPromoted::work（zRelocate.cpp:1257-1280）用它 object_iterate 完、relocation set reset 时释放。我方对应：PromoteYoungRegion 时把晋升前的页元数据（原 LiveInfo 本体、top/start、age）**移交**给 flip_promoted_pages 的元素对象（原 young 页视图，拥有原 livemap，⛔ 不复制位图），region 槽位装上 old 身份；该视图的生命周期到 flip-promoted remset worker join / relocation set 重置为止再释放。这是 ZGC「原 young 页对象继续存在」的拥有关系，不是 renamed retained snapshot（区别：无副本、无回读、生命周期与 ZGC 同点结束）。
2. RememberFlipPromotedPages 改为对该视图 object_iterate（对应 :1268-1276），删掉 retained 位图副本及回读路径全链。
3. 命名/文件按 ZGC（zRelocationSet 持有 flip_promoted_pages；页视图类型名自定但要在对应表标明它对应「clone_for_promotion 之后的原 young ZPage」）。⛔ 不引入页表映射重排、不改 UnitInfo 数组布局——那部分入表标「ZGC 页对象模型 vs 我方 region 槽位模型，基础设施差异」。
