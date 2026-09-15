# P2 接续，产品修改前顺序表
坐标：候选 ec8acc48fffd6badb68c6736ee25907216359fc5 + main 2677e74382846076984b0adf4e5df5e953b46dad（merge处理中）。
| producer → consumer | 修改落点 | ZGC |
|---|---|---|
| zMark.cpp TraceObjectReferences/TraceArray → TraceRefField → MarkBarrierOnOldOopField | 在字段屏障内按finalizable选择fast/slow/color，删除TraceFinalizableRefField旧staging | zMark.cpp:198; zBarrier.inline.hpp:626 |
| zBarrier.cpp WriteReference → StoreBarrier → zStoreBarrierBuffer.cpp OnNewPhase/Flush → RememberedSet.Record → zGeneration.cpp StartMark flip → DoYoungGarbageCollection RescanRememberedSet → zRemembered.cpp RemsetBarrierOnOopField → MarkYoungSlowPath → young MarkObject → young字段闭包 | 原接线保留，公共颜色使用P01 ZAddress API | zRemembered.cpp:578; zBarrier.inline.hpp:681 |
| zGeneration.cpp young mark roots → zMark.cpp young follow字段 → MarkBarrierOnYoungOopField → MarkFromYoungSlowPath | current校验后判young/major roots；普通root不迁入 | zBarrier.cpp:158 |

T5计划：真实分配/store/注册根/GC建图，观测槽字、对象mark与follow结果；Strong/Finalizable old→young慢路无结果/无heal及old→old阳性，young→old major/nonmajor门；真实remset后续接管。保留fast轴。生产/相位消费切刀沿原四臂；旧报告不作新SO验收证据。

## 0916 advisor最小生产接缝（先读后改）
授权 /root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_607_implement_r5684610492-20260915T172100Z.md；规格 REPORT-release002_finalizable_phase_0916.md §3–5。
原AllocateFinalizerHandle/RegisterFinalizer → weakStorage权威槽 → MarkOldRootsTask自有ParState<true>同周期一次枚举 → ReadStaticRef(仅load，不Strong mark) → current资格 → should_discover(old且未Strong) → 原ReferenceProcessor discoveredList一次性声明 → root Finalizable barrier → old MarkDomain → field Finalizable slow → 原任务FlushMarkStacks/TracingImpl/TryEndOldMark → ProcessReferences最终Strong/live分类 → rendezvous/unblock/enqueue。
重复发现沿原discoveredList CAS重试查重；不新增对象表。旧non-strong DoResurrection调用/定义/声明删除，不改IsPhaseMark。后续P13整体替换数据形态时接收该生产/终止合同。
