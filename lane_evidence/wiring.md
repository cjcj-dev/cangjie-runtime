# 修改前 producer → consumer 顺序
- young pause_mark_start (runtime/src/Heap/z/zGeneration.cpp:376) → VM_ZMarkStartYoung pause → mark_start (:394) → object_allocator.retire_pages (:406) → 每 age 清共享页 (:283-285)。检查必须位于每 age 清页之前，对应 ZGC PerAge::retire_pages:196-201。
- old mark_start (runtime/src/Heap/z/zGeneration.cpp:842) → object_allocator.retire_pages (:851) → 同上。
- Heap::unsafe_max_tlab_alloc (runtime/src/Heap/z/zHeap.cpp:471) → fast_available (:261) → shared_small_page_addr (:263)。线程身份检查应在读取共享页地址之前，对应 ZGC :227（待 advisor）。
