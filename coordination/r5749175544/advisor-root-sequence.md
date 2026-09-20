LANE=sym_cangjie_runtime_759_implement_r5749175544
ROLE=implement
问题：#759 生产端查到跨代根标记与 young mark-start 缺少 ZGC 同形同步；请求确认修复范围与转红判据。
已做：移除 relocate-start 后三处 early return、根有表统一 relocate_or_remap，提交 1e125e00763ab406d4d1749afec27a6d215cef14；两构型 rc=0。medium 仍返回已占用页起址并在同签名失败。
实测（kkk2:/root/sym_cangjie_runtime_759_implement_r5749175544-initial/targets/watch-gdb.log）：硬件 watchpoint 显示大页 livemap seq=0→初始化→2（MarkYoungSlowPath）→0（ZFlipAgePagesTask::work）；随后下一次 young select 判空并 free_page，页 age=survivor1 birth_seq=2，live_objects=1/live_bytes=534773760 仍是上一轮；下轮没有新 mark 位写入。release-gdb.log 的真实链=young select→free_empty_pages→free_page→medium alloc 复用→PreforwardDiscoveredExternObjects 解析 cycle map 非 Usable。
源码：ZGC zMark.cpp:939-942 mark_old_roots 在调用层 SuspendibleThreadSetJoiner；我方 ZMark::DoEnumeration zRootsIterator.cpp:201-206 未 join（young 已在 zMark.cpp:386 join）。ZGC zCollectedHeap.cpp:339-349 safepoint_synchronize_begin/end 包含两代 synchronize_relocation 与 STS synchronize；我方全 runtime/src 只有 ZRendezvousGCThreads 的短暂同步，VM_ZOperation::pause zGeneration.cpp:189-204 仅 ScopedStopTheWorld；这是正在执行 old root barrier 时 young 可翻色/升序、旧操作用新色 self-heal 而新标记未完成的窗口。尚不把这个推理当已闭环结论。
请求：允许本条生产端修复包含 ZCollectedHeap safepoint 同步接线/old root join（按 ZGC 相同函数分解，可能涉及 MutatorManager 的 STW 入口），并构造产品结果敏感的该面转红。原验收限定切回1或2必须重现这两个既有用例，尚无证据可达：实际生产面可能是第三项跨代页存活。能否增加/替换为该同步承重点精确转红？
当前报告保持 WIP；继续完成不依赖答复的 root/phase 同步对应与测试夹具核对。
