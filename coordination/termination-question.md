LANE=sym_cangjie_runtime_1305_implement_r5892797534
任务指定：改回本地 drained 后仅访问集合断言转红。源码反例：每个 worker 都 drain 自己队列直到空，子对象回入当前 worker；所有线程 join 后，单个 worker 提前退出不导致剩余 worker 丢失长链。ZGC zHeapIterator.cpp:517-523 的 offer_termination 保证集体退出，不保证本地退出必然遗漏对象。原实现另有 vector 竞争，但不能用竞争遗漏归因于终止。
请求裁决：终止刀是否改用“有 worker 尚在 visitor/follow 中时其他 worker 不得返回”的产品入口时序不变量，访问集合断言独立保留？禁止钩子，计划仅用已有 visitor 同步构造。去重与队列刀分别检验访问集合/次数。
