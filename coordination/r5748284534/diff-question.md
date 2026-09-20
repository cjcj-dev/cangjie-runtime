LANE=sym_cangjie_runtime_627_implement_r5748284534
按 15:1x 裁决已接回 eab957d9，候选 8d9ef77f66bc，两构型 rc0，四臂 ran；DIFF-8d9ef77f66bc-vs-eab957d9ff92 的 testable 有8独红（其他臂0）：NativeRootCurrent.MajorSeed/StrongFinalizerRootPublishesAndMarks/YoungGoodMarksBeforeHealingAndSkipsRepeat、ThreadRootCurrent.C1-C4、ZVerify.WeakFieldRejectsUnmarkedYoungTarget。
前7项因冲突保留 P16 夹具 NativeRootTrace/手工 mark start，而主线 #717 已改真实 old.mark_start/old.concurrent_mark 与 young.mark_start（P16 断言槽/活位仍应保留）。拟仅接入主线这些真实相位夹具初始化，不恢复回调/private snapshot，不弱化断言；ZVerify 项先核原始日志。
此处已超过原“只删3文件和记账”的净范围，但系强制接回产生。请确认按此整合；本棒继续定位、不终态。
