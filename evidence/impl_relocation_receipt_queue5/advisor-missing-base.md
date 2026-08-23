# impl_relocation_receipt_queue5：当前 main 缺少 9b749502 的产品依赖

任务书要求在 `c1885ff6b338ac11fa2e43b93402be0535449156` 上仅 cherry-pick `9b7495028bcb513c5f6bc911cf2433a7a6798755` 并解冲突。

实测：

```text
$ git grep -n 'RelocationRequestQueue' HEAD -- runtime/src runtime/tests/gc_unit
# rc=1，零输出

$ git grep -n 'RelocationRequestQueue' 9b7495028 -- runtime/src runtime/tests/gc_unit
runtime/src/Heap/Collector/RelocationRequestQueue.cpp:15:void RelocationRequestQueue::BeginCycle()
runtime/src/Heap/Collector/RelocationRequestQueue.h:28:class RelocationRequestQueue {
runtime/src/Heap/Allocator/RegionManager.h:27:#include "Heap/Collector/RelocationRequestQueue.h"
...（多处产品消费者与测试）
```

`9b749502` 的单提交 diff 不含 `RelocationRequestQueue.{h,cpp}`，因为这些文件已在其父提交 `fce4b83c...`；当前 main 却没有它们。单独 cherry-pick 已发生预期的五文件冲突，但即便解完，也会引用不存在的队列源码，无法编译。

请裁决范围：是否允许从 `9b749502` 按功能接回其缺失的产品/测试依赖（以 `git diff c1885ff6..9b749502` 机械列举后逐项归并），还是有另一枚已钉定的祖先提交应先接回？在裁决前我会继续完成五个当前冲突文件的三方分析，但不擅自扩大接回范围。
