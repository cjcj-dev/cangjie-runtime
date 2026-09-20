待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# #717 producer → consumer（改码前源码核对）
坐标 b6d62daa8f3557c8a9effbfa4709a4744e315497。

| Producer | Consumer | 修改位置 |
|---|---|---|
| zGeneration.cpp:481 epoch_id → :484 GcPhaseEnum | Mutator.cpp:858 finish_processing → zStackWatermark.cpp:129 IsDone/:137/:157 finish_processing | 保留幂等状态；不在调用层判断完成 |
| zMark.cpp:387 VisitMinorRootSlots | zMark.cpp:185 VisitStrongPlainRoots → :197 VisitMutatorRoots | 此线程闭包改为 finish_processing，完成判断属于被调函数 |
| zStackWatermark.cpp:103 process_head | :77-78 VisitExceptionRoots / VisitNativeFrameRoots → 槽 visitor | 消费端切刀位置 |

ZGC zRootsIterator.cpp:126-137 逐线程调用；zMark.cpp:703-708 do_thread 无条件 finish_processing；runtime/stackWatermarkSet.cpp:141-146 转交 watermark。
我方无 return statepoint，完整扫栈位于 finish_processing；epoch 是并发收据。Mutator.cpp:852-858 没有 IsDone 早退，epoch==0 是既有直接扫描入口，不是完成态分路。
