# #740 处置与 ZGC 对应
待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标基于 b6d62daa8f3557c8a9effbfa4709a4744e315497；ZGC 根 `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`。

## 名单处置
T=测试布置错误：产品 SO 完全不变，调整 reset→mark fixture 顺序即转绿，保留原 prev/new/remset/domain 断言。

| 测试 | 处置 | 原失败 |
|---|---|---|
| StoreBuf.ProductWriteCarriesOldValueOnlyInPrevArm | T | retired.size=0 |
| StoreBuf.ProductPhaseFlushHandsPairedPrevToMark | T | oldCount=0 |
| StoreBuf.CompilerStoreBadOverwriteHandsObservedOldToMark | T | oldReceipts=0 |
| BarrierOldAtomic.NoAllocBufferOverwriteRetiresOldValue | T | receipts.oldValue=0 |
| BarrierOldAtomic.AllocBufferOverwriteRetiresOldValueControl | T | receipts.oldValue=0 |
| BarrierOldAtomic.ReflectionStaticAggregateStoreRetiresNativeOldValue | T | oldValueRetained=false |
| YoungConc.RemovingExportRootPublishesPreviousValue | T | work.size=0 |
| YoungConc.BulkWritePublishesSatbWithoutYoungRegions | T | work.size=0 |
| YoungConc.TraceStorePublishesPreviousYoungTarget | T | work.size=0 |
| YoungConc.MarkEndDomainContainsPublishedYoungWork | T | YoungPending=0 |
| YoungConc.StoreBufferFlushPublishesYoungMarkWork | T | YoungPending=0 |

前六项 default/testable 两构型，后五项 testable，共17个原红组合。证据：kkk2:/root/sym_cangjie_runtime_740_implement_r5747608277-baseline/reproduction/results.json 与 corrected-{default,testable}/run.log；基线 SO sha 两臂分别 c42702991b654b6f810d3fe06717f004c3159cca57eb133880526474c89bb485 / fda53a6d170536957d90ef7e660374a218f0ee1edaa9d9d3aeb9342ac5e17125。

## 对齐表
| ZGC | 我方 | 本轮 |
|---|---|---|
| zPage.inline.hpp:180-185；zMark.inline.hpp:51-55 | zPage.inline.hpp:703-710；zMark.inline.hpp:19-21 | allocating 分路正确，保持；测试页 reset 必须先于 mark start |
| zBarrier.inline.hpp:695-717；zBarrier.cpp:253-278 | zBarrier.cpp:262-290、329-345 | prev 慢路生产已在场，不改 |
| zStoreBarrierBuffer.inline.hpp:37-47 | zStoreBarrierBuffer.inline.hpp:8-15 | 配对入队已在场，不改 |
| zStoreBarrierBuffer.cpp:263-281 | zStoreBarrierBuffer.cpp:158-166 | Flush 统一调用 mark_and_remember；形态收口，不是17条红的修复 |
| zBarrier.inline.hpp:730-751 | zBarrier.inline.hpp:357-373 | mark_and_remember 根据 referent 代路由，再记 old slot；保持 |
| zStoreBarrierBuffer.cpp:201-244 | zStoreBarrierBuffer.cpp:120-155 | on_new_phase 条件与顺序保持 |
| zMark.cpp:998-1004 | zMark.cpp:1193-1201；Mutator.h:497-502 | 先 store buffer、后 mark stacks；本包未改相位路由 |

## 删除清单
删除 Flush 内展开的 MarkObjectIfActive + remember 重复路径，及无 ZGC 对应的逐条 buffer[i] 清零（逻辑 clear 保留）。不删除 API/类型，不保留开关或旧路由。`deletion-and-preservation.txt` 给基线阳性对照和候选检索输出。
本地参考 JDK gtest/gc/z 未提供 test_zStoreBarrierBuffer；不冒称移植了不存在的测试，直接用真实产品入口覆盖相同状态义务。

## 测试集合差
新增 StoreBuf.AllocatingPreviousValueIsImplicitlyLive（old/young allocating 对照）、StoreBuf.ProductWriteFlushPublishesPreviousYoungValue（真实 WriteReference→mutator flush→young 栈→prev/new/remset 断言）。删除/改名集合为空；11项只调整布置，未改期望。

## 红臂范围
基线刀：WriteReference→StoreBarrier、StoreBarrier 慢路 buffer->add、remember→page->remember；联合 entry_cut_check 验证真实入口及消费者。统一 Flush 新行刀按 advisor 20260920T043454Z 单列例外，不冒充基线行。
正常/四刀/恢复都用同构型同一个最终测试 ELF，仅 libcangjie-runtime.so 改变，libboundscheck 相同；恢复 SO sha==正常。每臂 default 34项/testable 42项。实际注册表导出名单；首轮 source regex 把 default 不编译的 YoungSlotExcludedFromOldPhaseSnapshot 算入，已保留初次装置错误并改为 ELF 注册清单。
红臂目录 kkk2:/root/sym_cangjie_runtime_740_implement_r5747608277-green/matrix-final；当地副本 local:/root/cj_build/reports/EVIDENCE-sym_cangjie_runtime_740_implement_r5747608277/matrix-final。
