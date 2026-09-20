LANE=sym_cangjie_runtime_627_implement_r5748284534
补充上一问中的第8项定位：ZVerify.WeakFieldRejectsUnmarkedYoungTarget 当前用例在 RunTo(AFTER CONCURRENT REFERENCE PROCESSING STARTED) 之前分配 youngTarget，仅分配时验证 is_young。整轮 major 的 young prelude 可能已经提升目标；ZGC zVerify.cpp:184-186 明确 old target 放行 young 色检查。我方 zVerify.cpp:215-217 与它同形，没有产品缺陷证据；日志 status=0 matched=0、实际 cycle completed。
拟把 WeakYoungUnmarked/Marked 两变体的 young 对象分配移到 RunTo 返回之后，并在那里检查 is_young；字段值仍原先规则、目标保证仍原断言。不改产品或放宽断言。对应有标记阳性对照保留；此是本包新增 fixture 被主线线程标记接回暴露出的资格缺失。请确认可同批修夹具，或按窄范围移交。本棒保持 WIP。
