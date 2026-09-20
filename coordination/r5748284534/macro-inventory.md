# 全量宏清单

head=8d9ef77f66bc3f82cab8f998879ce48c7539ac02；范围 runtime/src/Heap（Allocator 与 z 均包含）。

8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zGeneration.cpp:552:#if defined(MRT_TESTABLE_INTERNALS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zGeneration.cpp:1656:#if defined(MRT_TESTABLE_INTERNALS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zGeneration.hpp:63:#if defined(MRT_TESTABLE_INTERNALS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zIterator.inline.hpp:56:#if defined(MRT_GC_UNIT_TESTS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zMark.cpp:480:#if defined(MRT_TESTABLE_INTERNALS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zMark.cpp:898:#if defined(MRT_TESTABLE_INTERNALS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zMarkStack.cpp:26:#if defined(MRT_TESTABLE_INTERNALS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zMarkStack.cpp:280:#if defined(MRT_TESTABLE_INTERNALS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zMarkStack.hpp:162:#if defined(MRT_TESTABLE_INTERNALS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zObjArrayAllocator.cpp:58:#if defined(MRT_GC_UNIT_TESTS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zObjArrayAllocator.cpp:71:#if defined(MRT_GC_UNIT_TESTS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zObjArrayAllocator.cpp:89:#if defined(MRT_GC_UNIT_TESTS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zObjArrayAllocator.cpp:135:#if defined(MRT_GC_UNIT_TESTS)
8d9ef77f66bc3f82cab8f998879ce48c7539ac02:runtime/src/Heap/z/zStackWatermark.cpp:87:#if defined(MRT_GC_UNIT_TESTS)

残余按本轮只两条范围保留：MarkClosureObserver 与 testYoungMarkCompleted 归 #736；zObjArrayAllocator 分段 hook 已审认可保留；zStackWatermark、zIterator 的条件检查为冻结实现，本轮不改。不是全树宏零命中声明。
