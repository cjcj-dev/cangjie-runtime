LANE=sym_cangjie_runtime_496_implement_r5656148950
已合入 403916767。逐函数定位发现三点：
1. 旧代 DoTracing 仍在 Heap/z/zMark.cpp:1716，实际 concurrent mark 前后通知需要在此接线，或把该 orchestration 函数移到 zGeneration.cpp（对应 ZGenerationOld::collect）；请求授权此函数。
2. GC_REASON 尚无 _wb_breakpoint 对应，需在 Heap/Collector/GcRequest.h/.cpp 增加 GC_REASON_WB_BREAKPOINT 原因及表项，并在 driver 专用分支 start_gc + major port async，不是开关；请求授权。
3. remap_window_fixture.hpp 是十余个 forwarding retain/wait/copy/cleanup 细粒度 callback 场景，不是三个 ZGC old-mark phase 能替代。参考 ZBreakpoint 只有 AFTER MARKING STARTED/BEFORE MARKING COMPLETED/AFTER CONCURRENT REFERENCE PROCESSING STARTED，硬把这些 relocation 断点改为新名字属于发明。请裁定：删除无 ZGC 对应的 remap fixture 场景并同批移植 ZGC TestConcurrentGCBreakpoints 场景（不改期望值、不假装覆盖原 forwarding 场景），还是保留独立用途 callback 延后至其归属机制包？当前先实现已授权 shared/ZBreakpoint 协议。
