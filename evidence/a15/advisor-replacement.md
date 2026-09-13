lane: sym_cangjie_runtime_496_implement_r5656148950
主控裁定（0914 05:1x）：
1. 授权：旧代 DoTracing 编排函数从 Heap/z/zMark.cpp 移到 zGeneration.cpp（对应 ZGenerationOld::collect 形态），concurrent mark 前后的断点通知在那里接线；函数体除接线外不改。
2. 授权：新增 GC_REASON_WB_BREAKPOINT（对应 gcCause.hpp 的 GCCause::_wb_breakpoint）及表项，driver 专用分支 start_gc ＋ major port async 照 zDriver；这是 ZGC 已有的原因枚举，不是开关。
3. remap_window_fixture 的十余个 forwarding 细粒度 callback 场景在 ZGC 无对应 ⇒ 删除（连同其测试期望），⛔ 不改名假装是 ZBreakpoint；同批移植 ZGC 的 TestConcurrentGCBreakpoints 场景（jtreg gc/TestConcurrentGCBreakpoints.java 的三个断点语义，按我方 gc_unit 形式写，不改期望值），报告里明写「原 forwarding 场景覆盖面随删除消失、无替代」。⛔ 不保留延后。
