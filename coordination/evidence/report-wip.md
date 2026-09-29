PROGRESS=WIP · verdict=源码核对进行中，终止转红判据已询问 advisor ｜尺=read N=0 · LANE=sym_cangjie_runtime_1305_implement_r5892797534
DELIVERY_REF=none|no-code|实现进行中
SIDE_EFFECT: 已提交 advisor 问询；未推送
ROLE=implement
EVIDENCE=local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/termination-question.md
LANE=sym_cangjie_runtime_1305_implement_r5892797534

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
冻结回读：git -C /root/cj_build/cangjie_runtime rev-parse cjcjdev/main，rc=0，9733dfc09d29eca27d19cc3937a149838b4e3322。

## producer→consumer
| producer | consumer | 修改位置 |
|---|---|---|
| zHeapIterator.cpp:190 roots / :111 field closure | :58 context.push → :37 try_set_bit → queue | 发布队列前原子去重 |
| :68 push_array_chunk | :83 pop_array_chunk / :226 steal | 跨线程队列访问 |
| :212 drain → :226 steal | :252 drain_and_steal | local drained 判断之后 offer_termination |
| zHeap.cpp:566 object_iterate / :572 verify | zHeapIterator.cpp:277 object_and_field_iterate | 真正运行时入口；现有调用 nworkers=1 |

CLAIM: 本地退出缺少 ZGC 全体 offer 协议，跨 worker vector 操作未同步。
  METHOD: read
  EVIDENCE: runtime/src/Heap/z/zHeapIterator.cpp:226; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zHeapIterator.cpp:517

## FALSIFIED
任务要求“改回本地 drained 必然让访问集合不等”尚无依据：每个 worker 完成自己的递归工作且全部 join 时，提前退出空闲 worker 不意味着遗漏。已向 advisor 提问，未改验收期望。
问询文件：/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_1305_implement_r5892797534-20260929T145851Z.md。

## 进展（尚未验收）
advisor 已答复：/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_1305_implement_r5892797534-20260929T145851Z.md。
原任务书主控判据错误；批准终止刀改为“任一 worker 仍有工作时其他 worker 不返回”，访问集合和去重次数独立检查。
已提交 ZGranuleMap acquire/recheck/release、共享根迭代状态、owner/steal/overflow 队列、TaskTerminator；没有保留旧路径开关。
初始双构型 a7932050e1 rc=0/0，后续 3b4fb683f1 rc=0/0（39s，两臂各 -j48）。最新源码仍待完整验收。
测试编译两次 rc=123：NativeSlot 默认构造被删除。首次误判为不可复制，改为 vector(count) 后仍失败；已核 RefField.h:148-156，改用显式 null 值初始化，待验证。编译失败不计转红。
交付前 fetch+merge：cjcjdev/main 仍为 9733dfc09d29eca27d19cc3937a149838b4e3322，merge rc=0（Already up to date）。

## ZGC 对应（源码实施记录，不是自审放行）
| 我方 | ZGC 锚 | 变化 |
|---|---|---|
| HeapIterator::object_bitmap / mark_object | zHeapIterator.cpp:312-347 | granule map 查询/安装与 bit CAS 分离 |
| HeapIterator::mark_visit_and_push | zHeapIterator.cpp:421 | 去重在入队前，verify 时机在 mark 后 |
| HeapIteratorContext::pop / pop_array_chunk | zHeapIterator.cpp:107-116 | overflow 优先，再 owner pop |
| HeapIterator::steal 两重载 / steal_array_chunk | zHeapIterator.cpp:497-514 | 数组优先，偷到即 follow；对象其次 |
| HeapIterator::drain_and_steal | zHeapIterator.cpp:517-523 | drained 后 offer_termination |
| GenericTaskQueue / TaskQueueSuper / OverflowTaskQueue | gc/shared/taskqueue.hpp:153,333,437 | tagged age + owner bottom + bounded ring + owner overflow |
| GenericTaskQueueSet | gc/shared/taskqueue.inline.hpp:315-391 | best-of-two、上次 victim、两 worker 分路、2*n 重试 |
| TaskTerminator | gc/shared/taskTerminator.cpp:39-218 | offered count、spin master、timed wait、全体退出 |
| ZGranuleMapIterator | zGranuleMap.inline.hpp:117 | 复用 ZArrayIteratorImpl 回收位图 |

C++ 容器替代 HotSpot 存储：位图 words 用 atomic word vector（ZGC CHeapBitMap）；overflow 用 vector（HotSpot Stack）；queue pointer table 用 vector（HotSpot T**）。不以功能等价代替分路对齐。
