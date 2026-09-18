# Final class fold B — relocation / colored slots

基线读取：379aae5a8。独占修改仅 zRelocate.hpp/cpp、zBarrier.hpp/cpp；无 build/commit/push。

## 已实施及源码锚

- zRelocate.hpp:140 起声明真实 ZRelocate static 入口：ForwardFromSpace、RefineFromSpace、ForwardObject、ForwardObjectExclusive、IsFromObject、IsUnmovableFromObject、ResolveMinorReference 两重载、FixMinorEvacuatedSlot 三重载、FixMinorRootSlots、RemapYoungRoots、Preforward、StartRelocationTasks。参数保持原契约，去掉旧实例 const。
- zRelocate.cpp:107/123 的 ForwardFromSpace/RefineFromSpace 从基线 zGeneration.cpp 读取迁入；原定义由父棒删除。EvacuateYoungRegions 原定义从本文件删除，由父棒迁入 ZGenerationYoung。
- zBarrier.hpp:40 起承接 RefSlotKind、GetAndTryTagObj、TryUpdateRefField、TryUpdateRefFieldImpl、CasInstallResolvedTarget；GetAndTryTagObj 从基线 zMark.cpp 读取，A 棒负责删除旧体。
- `RootSlotWriteback` 原为别名，消费直接改 `ZBarrier::GetAndTryTagRefField`；`IsGhostFromObject` 原为别名，消费直接改 `ZRelocate::IsFromObject`，未迁入新别名。
- zRelocate.cpp:334/416/697 三 helper 删除 HeapGcState receiver 参数；内部调用同步。RemapYoungRoots/Preforward/FixMinorRootSlots 调 ZMark::VisitStrongPlainRoots；worker直接读真实Heap代Workers。
- zRelocate.cpp:491/646/657 的 stack provenance 改为稳定真实 owner `&Heap::GetHeap().young().relocate()`，没有置空。
- 原 g_minorRefCasFail/Ok 从 zRelocate.cpp 匿名namespace迁入 zBarrier.cpp 唯一定义，zBarrier.hpp extern 给父棒迁入 generation 的原reset消费者使用；未增加新计数或豁免。
- zRelocate.hpp 保留原 MRT_TESTABLE_INTERNALS 下的 RelocationReceiptTestAccess/MutatorPublishTestAccess friends，删除旧 HeapGcState friend。

## 验证边界

`git diff --check` rc=0。检索 `HeapGcState|GetCollector\(|RootSlotWriteback|IsGhostFromObject|const_cast<.*this` 在本四文件未命中；基线中对应定义/调用有命中，命令：`git grep -n HeapGcState 379aae5a8 -- runtime/src/Heap/z/zRelocate.cpp runtime/src/Heap/z/zRelocate.hpp runtime/src/Heap/z/zBarrier.hpp`。此为局部源码删除证据，不替代产品构建/三臂或最终全树清零。

测试 agent 已按新静态接口迁移，尤其 FixMinorEvacuatedSlot 三签名、ForwardObjectExclusive、ForwardFromSpace、RemapYoungRoots、IsUnmovableFromObject、TryUpdateRefField；不需要追加兼容入口。

## FALSIFIED

无。
