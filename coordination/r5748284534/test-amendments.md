待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。

| 测试 | 合并问题 | 最终驱动/资格修正 | 断言 |
|---|---|---|---|
| NativeRootCurrent.MajorSeed、ThreadRootCurrent.C1-C4 | P16 手工 start 未接回 #717 真实周期起点 | old.End→mark_start→concurrent_mark，取消额外 PreparePlainRoots | 原槽回写与 current/stale 活位不变 |
| NativeRootCurrent.StrongFinalizerRootPublishesAndMarks | 同上 | 使用同一真实 old 相位 | marked 目标不变 |
| NativeRootCurrent.YoungGoodMarksBeforeHealingAndSkipsRepeat | 手工 start 未设置真实周期状态 | young.mark_start | 标记、发布一次、重复不增断言不变 |
| ZVerify.WeakFieldRejectsUnmarkedYoungTarget / WeakFieldAcceptsMarkedYoungTarget | youngTarget 在前奏前分配，检查前可能已提升 | RunTo 后分配并确认 is_young | 原颜色规则与匹配诊断不变；ZGC zVerify.cpp:184-186 |

相对4cc8e34a无测试名删除；新增 YoungThreadRoots 两项来自主线 #717。相对主线的历史删除分类引用 P16-deleted-tests.tsv 与 base-only-prior-classification.tsv，不计作行为修复。
