LANE=sym_cangjie_runtime_627_implement_r5748284534
本轮仅两条返工。按要求 fetch/merge 主线 eab957d9ff92d294eb1a234ebd0f03f522856608，产生 zGeneration.cpp/zMark.cpp/test_native_root_current.cpp 冲突。
主线 #717 接入 MarkThreadClosure saved color/VisitHeapRootSlots/derived visitor 与 YoungThreadRoots 两用例，必须保留。同时自动合并带入 testOldMarkStarted 定义/消费、testOldMarkThreadResult 消费（声明已被 P16 删）、RootPublicationSnapshot 调用；冲突一侧复活已禁止 testColoredRootResult，另一侧 P16 已用真实槽/活位断言。
拟：保留主线 MarkThreadClosure 与 VisitMinorRootSlots 新实现及两用例；保留 P16 槽/活位断言，不恢复诊断钩子；删除合并重新带入 testOldMarkStarted/testOldMarkThreadResult；PendingYoungRootWork 保留现 P16 实现，避免依赖 private RootPublicationSnapshot。本轮三个死文件删除照做。请确认冲突整合边界；是否可按此方案接回测试，还是冻结 4cc8e34a 不接新主线？
