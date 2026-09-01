# impl_gcunit3：T12 窄差前提被实测证伪，T13 命中结构不可满足位

当前基线 `aa48a3cec46a97076718a6f5da6cc1d77411a48b`，构型 `MRT_GC_UNIT_TESTS=ON,MRT_TESTABLE_INTERNALS=ON,Release`，kkk2 核域 `16-31`。

## 实测

基线七项逐项均真实启动、rc=1。runtime SO=`41682a5c3e5bc38bdb4010490df4f352d55e114cb0aff23b2cb159c32756b074`，test ELF=`4ed80131fb2eece08734d14f2322bcb79cc12e9c49a6e3be27d5f332152d4cf0`。

我严格只把任务书指定的两处 Young 请求从 `false` 改为 `true`（另叠加独立 T11 测试改写，不接触 segmented 产品行为），同构型重编产品 SO/测试 ELF 后：

- 两个 T11 Young 项仍 rc=1；
- serial 从 `status=1 gc_before=2 gc_after=3 root_sites=0xa5` 退化为 `status=4 gc_before=2 gc_after=2 root_sites=0`；
- parallel 从 `status=1 gc_before=2 gc_after=3 root_sites=0xa5` 退化为 `status=5 gc_before=2 gc_after=2 root_sites=0`；
- 候选 runtime SO=`c9f1ed2c4e52830b34eb0e451eeeecba635a464478c4fcdecc2b126803256103`，test ELF=`aea91fc682071e576ee9b4f9c25b6ef973873ba32f4f2694282e232335d2c010`。

源码原因：当前 `CollectorResources::RequestGC(..., true)` 在 `runtime/src/Heap/Collector/CollectorResources.cpp:401-403` 只入异步队列后返回；任务基线不含历史 `2880abc5d8` 的 async receipt/`WaitForAsyncGCFinish`。当前两处调用的下一步分别是 `MArray.cpp:154` 立即检查 epoch、`test_segmented_array_init.cpp:217-223` 恢复 managed context/移除 seed/读取 epoch，没有等待该请求完成。因此“只重落两行即可绿”在 aa48 基线上不成立。

T13 也已定位：FULL 实得 `root_sites=0x84`，`test_segmented_array_init.cpp:413-417` 要求 `0xc4`，缺的 `0x40` 是 `REMEMBERED`。该位唯一生产点为 `runtime/src/Heap/Collector/Remembered.cpp:545`，位于 `InvalidateOldTaggedRefs`；当前产品调用检索只有 `Remembered.cpp:496` 的 `InvalidateOldTaggedRefs(true)`，且 `:487-495` 默认因诊断 env 未开直接返回。`InvalidateOldTaggedRefs(false)` 产品调用点为 0。因此默认 ON:ON 下要求该位结构不可满足。Young 实得 `0xa5`、要求 `0xb1`；`Mark.cpp:803-807` 在 watermark 已完成时跳过 mutator，而实际日志明确 `concurrent_done=1`，所以“必须同时出现 MINOR_MARK”也不是稳定不变量。

## 请求裁决

请在以下范围中裁决：

1. 是否撤销 T12 两行窄差（保留当前同步 receipt），改为修正 segmented 测试的过时“固定消费者位全集”判据，使它按合法替代路径断言不变量，并另做产品承重点断线臂；
2. 或授权扩大 T12 范围，恢复请求绑定 async receipt + wait 后再保留两行 `true`（这明显超出任务书“窄差两行”）；
3. T13 是否授权删除默认不可达的 `REMEMBERED` 必选位，并把 FULL/Young 的消费者要求写成合法路径的替代集合，而不是统一 managed/native FULL 位集。

在 outbox 答复前报告保持 `PROGRESS=WIP`；我会继续完成独立 T11 红/恢复臂，不提交两行 async。
