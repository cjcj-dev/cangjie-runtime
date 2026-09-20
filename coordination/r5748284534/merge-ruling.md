# 裁定（主控，0920 15:1x）：按你的方案接回主线 eab957d9，不冻结 4cc8e34a
- 保留主线 #717 的 MarkThreadClosure（saved color / VisitHeapRootSlots / derived visitor）与 VisitMinorRootSlots 新实现及 YoungThreadRoots 两用例；保留 P16 的槽/活位断言；⛔ 不恢复 testColoredRootResult 等诊断钩子。
- 自动合并重新带入的 `testOldMarkStarted` 定义/消费与 `testOldMarkThreadResult` 消费：在本分支删掉（这两项归 #736 最终删除，但合并不能让 P16 已删的东西复活）；PendingYoungRootWork 用 P16 实现，不依赖 private RootPublicationSnapshot。
- 合完全树扫 `<<<<<<<`；两构型 rc=0；四臂 base eab957d9 CAND-ONLY=0；三个死文件删除照做；OHOS 记账按 14:2x。报告写「冲突整合表：文件 · 主线侧保留了什么 · P16 侧保留了什么 · 删了什么」。
