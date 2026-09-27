待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
补充上一问 sym_cangjie_runtime_1215_implement_r5855214347-20260927T105750Z.md：发现刚入主线 #1213/#1214 的直接消费冲突，尚未改产品。
冻结主线 cbadab0528 的 runtime/src/Heap/z/zStackWatermark.cpp:167-186：process_frame 复制 cursor.RegMap() 后 owner.process，再以该 incoming map 做 StackPtrMap::VisitReg，用于 sret 跨帧栈指针闭包。因此 StackFrameCursor.cpp:208 的 RecordCalleeSaved 不仅给 GC roots，也给下一帧 stackPtrRegRoot，test_pinroot.cpp:371-387 与 SretWatermark.UsesIncomingRegisterPointerMap:581-584 明确验证它。
按原计划删除 :208 会断主线新包 stackPtrRegRoot 的合法输入。这是否应保留独立的栈指针 prologue 数据消费（与 GC RegRoot 分离），还是配对 LLVM #87 已保证 stackPtrRegRoot 同样归零、因此该测试机制需随包删？请给生产端证据和授权边界，不能以零计数推导。
本棒继续 WIP；这是实测发现的前提不完备，不以复跑或弱化断言凑绿。
