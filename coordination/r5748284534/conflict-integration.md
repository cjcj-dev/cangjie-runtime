待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。

裁决：merge-ruling.md；主线 eab957d9ff92d294eb1a234ebd0f03f522856608。

| 文件 | 主线保留 | P16 保留 | 删除 |
|---|---|---|---|
| zMark.cpp / zMark.hpp | MarkThreadClosure saved color、VisitHeapRootSlots、derived visitor；VisitMinorRootSlots 无完成态早退；VisitMinorRoots 新签名 | 无诊断回调的根任务 | 冲突中的 gMinorRootOrigin、testOldMarkThreadResult 调用；旧重复闭包 |
| zGeneration.cpp / zGeneration.hpp | young 调新签名、old 根任务自行完成线程 | 已清扫的收据路径不复活 | testOldMarkStarted 声明/定义/调用（按裁决）；旧调用层完成态判定 |
| test_native_root_current.cpp | YoungThreadRoots 两个用例逐字保留 | 原产品槽/活位断言；PendingYoungRootWork P16 实现 | 不接回 testColoredRootResult、testOldMarkStarted 与 private snapshot 依赖；无测试删除 |
| implementation-717.md | 原样保留 #717 生产消费对应表 | 不适用 | 无 |

主线内容计数在 main-preservation.json；它不把计数当作语义证明。ZGC 线程闭包锚 zMark.cpp:703-708,827-828,883。
