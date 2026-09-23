LANE=sym_cangjie_runtime_917_implement_r5785846982
ROLE=implement
PROGRESS=WIP
任务标题要求 retire_pages/alloc_object 处 safepoint 与线程身份断言，但参考 /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zObjectAllocator.cpp:183-194 alloc_object 没有线程身份断言，:227-228 的断言在 fast_available。我方 runtime/src/Heap/z/zObjectAllocator.cpp:261 fast_available 无断言；:279 retire_pages 无 PerAge 分解与 safepoint 断言。
请求确认：按 ZGC 在 PerAge::retire_pages 加 WorldStopped 检查并恢复外层逐 age 调用；线程身份断言加在 fast_available（ThreadLocal::GetMutator()!=nullptr），不加在 alloc_object。是否批准按此精确锚实现？
