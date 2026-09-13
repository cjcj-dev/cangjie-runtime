待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# D07：名称与开关尾项清理

冻结基线 `d592d283e8ecc50f819db186824f3c78e64ae4e4`；产品提交 `f82d04fd367eb3f7866c7a0b387f7f5539ff9e15`。本轮为形态对齐交付，不含运行时门、unit 或切刀结果。

## ZGC 函数对应与保留边界

| 本轮涉及的我方生产/消费点 | ZGC 函数或开关 | 处理 |
|---|---|---|
| runtime/src/Heap/z/zMark.cpp:442 TraceObjectRefFields → FollowArrayElements | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.cpp:346 follow_array_object → :257 follow_array_elements | 删除开关选择的完整 inline 回滚路径，引用数组直接分块 |
| runtime/src/Heap/z/zMark.cpp:2088 FollowElements；:2125 FollowObjectReferences；:2139 FollowPartialReferences | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.cpp:216 follow_array_elements_large；:346 follow_array_object；:265 follow_partial_array | 保留已有分块算法、合法不可编码回退和 typed entry；删除 Enabled/计数/报告 |
| runtime/src/Heap/Collector/CopyCollector.cpp:105；GcStats.cpp:62 RecordMajorGCFinish | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDirector.cpp:876 sample_stats 按代采样 | 删除 young 更新 major 时钟机制，major 时间戳保留 |
| runtime/src/Mutator/MutatorManager.cpp:32、:533 RunEpochHandshake | /root/cj_build/reference/jdk/src/hotspot/share/runtime/globals.hpp:176；runtime/handshake.cpp:223 check_handshake_timeout | HandshakeTimeout 默认 0，条件 >0；无新 getenv |
| runtime/src/Heap/z/zStackWatermark.hpp:42 起产品状态方法 | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStackWatermark.cpp:175 start_processing_impl | 保留产品 state/epoch/phase 适配；删除自有 verify gate、日志和注入器 |
| runtime/src/Heap/z/zPage.inline.hpp:731 VerifyMarkFaceOwner；:944 NoteMarkEpochOnRead | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:119 generation_id；zLiveMap.inline.hpp:41 | 删除 count-only / epoch 诊断，保留默认 owner 与 epoch 判断；不声称逐指令等价 |
| runtime/src/Heap/z/zPage.inline.hpp:556 NoteRetainedPreserve；zPage.hpp:752 FORWARDING_FACE_RESET_BIT | ZGC 无独立对应 | 仅删除 probe-only 计数/枚举/getter，产品载体及位按 advisor 保留，归 D10 #503 |
| runtime/src/Heap/Collector/Collector.cpp:238 MarkGoodHeapGate；:249 PlausibleManagedObjectGate；TryRecoverInteriorBase | ZGC 无独立对应 | 删除自有日志/计数，产品根过滤按 advisor 保留，归 D10 #503 |
| runtime/src/Heap/z/zVerify.cpp:244 BeforeRelocation；:269 AfterRelocationInternal | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:531、:610 | 既有 remembered fields 校验原样保留；不是精确存活数校验 |
| 精确 live_objects/live_bytes 比较：本轮不补装 | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.cpp:196 verify_live | 自有 VerifyLiveBooks 日志删除；精确对应归 D10 #503，不能声称已有 ZVerify 覆盖 |

授权：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_493_implement_r5655121433-20260913T181501Z.md`、`…-20260913T181949Z.md`、`…-20260913T182428Z.md`。
共享 tools 不改；本包补丁取代仓内前置 D06b 的同名补丁（旧删除名册内容已包含在全清理补丁中）；`evidence/diag_registry.patch` 基于 `/root/cj_build/tools` 的 `2c5e3566491f97529149009cdf65b7d6b41f882d`，`git apply --check` rc=0，待主控在 D07 合入后应用。runtime 仓 `tools/trustp1_static_harness.sh` 为旧开关遗留产物，依裁定保留归后续清理。

## 完整旧清单逐项去向

旧清单有 72 行：71 个有后缀名和 1 个空后缀泛称；22 个可达 getenv 是旧坐标上的另一集合，不能当本冻结计数。使用 `MRT_GCV2_[A-Z0-9_]*` 枚举，本冻结产品词法集合为 23 项，候选为 0 项。锚列均为本冻结 `git show d592d283e8ecc50f819db186824f3c78e64ae4e4:<path>` 坐标。
“冻结前已无旧名”仅声明词法事实，不据此认领旧机制删除或声称功能不存在；未在本轮删除的自有产品机制按裁定归 D10。

| 旧名称 | 旧清单类别 | 本冻结实际锚 | 去向 | ZGC 锚 |
|---|---|---|---|---|
| `MRT_GCV2_` | 空后缀泛称（注释/日志） | `runtime/src/Heap/z/zStoreBarrierBuffer.hpp:19` | 删除泛称注释/分析器伪环境键 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_ALLOC_INTO_CSET_DIAG` | 注释/日志名称残留 | `runtime/src/Heap/Allocator/RegionSpace.h:178` | 删除 allocation-into-CSet 计数、报告和导出；保留分配退回路径 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_DIAG` | 真实getenv（字面） | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_DIAG_ACTIVE` | 真实getenv（helper间接） | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_DIAG_HELP` | 真实getenv（helper间接） | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_DIAG_SELFTEST` | 真实getenv（helper间接） | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_EMPTYLIVE` | 注释/日志名称残留 | `runtime/src/Heap/z/zPageAllocator.inline.hpp:62` | 删除自有空页 census 日志路径 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_EPOCH_HANDSHAKE_TIMEOUT_MS` | 真实getenv（字面） | `runtime/src/Mutator/MutatorManager.cpp:33` | 镜像 HandshakeTimeout=0 常量；删除 getenv 和旧 30000ms 默认值 | `/root/cj_build/reference/jdk/src/hotspot/share/runtime/globals.hpp:176` |
| `MRT_GCV2_F3_REGION` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_FROMVER` | 真实getenv（helper间接） | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_FWDDATA_GRACE` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_GARBREGION` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_HEALPAIR` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_INSTALLDOMAIN_ACCOUNT` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_LIVE_CROSSCHECK` | 注释/日志名称残留 | `runtime/src/Heap/z/zPage.hpp:1035` | 删除 VerifyLiveBooks 自有日志；精确 verify_live 对应归 D10 #503 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.cpp:196` |
| `MRT_GCV2_MARKCOMPLETE` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_MARKFLOOR_OBJ_GATE` | 注释/日志名称残留 | `runtime/src/Heap/z/zCollectedHeap.hpp:310` | 删除旧名、自有计数/日志；根过滤按裁定保留，归 D10 #503 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_MARKGOOD_HEAP_GATE` | 注释/日志名称残留 | `runtime/src/Heap/z/zCollectedHeap.hpp:300` | 删除旧名、自有计数/空报告；根过滤按裁定保留，归 D10 #503 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_MARK_EPOCH_ASSERT` | 注释/日志名称残留 | `runtime/src/Heap/z/zPage.hpp:428` | 删除观测计数和日志；保留 epoch 判断 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:41` |
| `MRT_GCV2_MASKEQUIV` | 注释/日志名称残留 | `runtime/src/Heap/Collector/Collector.h:387` | 删除死函数、声明及旧注释 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_MASKEQUIV_INJECT` | 注释/日志名称残留 | `runtime/src/Heap/Collector/Collector.h:388` | 删除死注入函数及旧注释 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_MINOR_DEFERS_HEU` | 真实getenv（字面） | `runtime/src/Heap/Collector/CopyCollector.cpp:110` | 删除 young HEU 延后状态、算法、调用和四个测试；保留 major 时间戳 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDirector.cpp:876` |
| `MRT_GCV2_MINOR_GC_ALOT` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_MUTRELOC_DRAIN` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_NULLROUTE_DIAG` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_NULLSLOT` | 真实getenv（helper间接） | `runtime/src/Heap/WCollector/WCollector.h:36` | 删除无实体对应的旧注释 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_ONESEQ` | 真实getenv（helper间接） | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_PARTIAL_ARRAY` | 真实getenv（字面） | `runtime/src/Heap/Collector/MarkPartialArray.cpp:39`、`runtime/src/Heap/Collector/MarkPartialArray.h:35` | 删除 Enabled 与完整数组回滚分支；默认分块直接接线 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.cpp:257` |
| `MRT_GCV2_PARTIAL_ARRAY_REPORT` | 真实getenv（helper间接） | `runtime/src/Heap/Collector/MarkPartialArray.cpp:50`、`runtime/src/Heap/Collector/MarkPartialArray.cpp:56` | 删除报告、原子计数、生产/消费记账调用与独立 TU | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.cpp:216` |
| `MRT_GCV2_PINNED_SCAN_PARALLEL` | 真实getenv（字面） | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_PROMO_DOMAIN_FATAL` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_PROMO_DOMAIN_FORCE_INPLACE` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_PROMO_DOMAIN_INJECT_UNDISCHARGED` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_PROMO_DOMAIN_RECONCILE` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_PROMO_DOMAIN_SKIP_ONE` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_REFFIX_INJECT_DISPEL` | 注释/日志名称残留 | `runtime/src/Heap/z/zPage.hpp:720` | 删除未消费的独立注入器 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_REGION_WAIT_DIAG` | 编译宏 | `runtime/src/Heap/z/zPageAllocator.inline.hpp:494` | 删除编译宏和全部日志块 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_RETAINED_OWN_COPY` | 注释/日志名称残留 | `runtime/src/Heap/z/zPage.hpp:1119` | 删除旧注释；默认 retained snapshot 载体按裁定保留，归 D10 #503 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_RETLIVE_PROBE` | 注释/日志名称残留 | `runtime/src/Heap/z/zPage.hpp:89` | 删除探针 getter、低位递增、last-op/clear 计数；保留产品位及载体，归 D10 #503 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_SCRUB_COST` | 注释/日志名称残留 | `runtime/src/Heap/z/zGeneration.cpp:605` | 删除空报告函数、声明、调用和导出 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_SKIPPED_WHO_MAX` | 注释/日志名称残留 | `runtime/src/Heap/Collector/TracingCollector.cpp:247` | 删除 SKIPPED_WHO 样本日志；保留 root-map miss 消费记账 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_SKIP_COMPACT_MEMSET` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACKREF` | 真实getenv（helper间接） | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACK_EXPOSURE_FATAL` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACK_EXPOSURE_HOOK` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACK_EXPOSURE_INJECT` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACK_EXPOSURE_VERIFY` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACK_FRAME_ORACLE` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACK_FRAME_ORACLE_FATAL` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACK_FRAME_ORACLE_SKIP` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACK_WATERMARK_FATAL` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACK_WATERMARK_INJECT` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_STACK_WATERMARK_VERIFY` | 真实getenv（字面） | `runtime/src/Heap/z/zStackWatermark.hpp:33`、`runtime/src/Heap/z/zStackWatermark.hpp:150`、`runtime/src/UnwindStack/StackWatermark.cpp:16` | 删除 getenv、自有诊断/注入器和专用脚本；保留水位产品状态 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStackWatermark.cpp:175` |
| `MRT_GCV2_TIPINHEAP_FATAL` | 注释/日志名称残留 | `runtime/src/Heap/Allocator/RegionInfo.h:122`、`runtime/src/Heap/z/zPage.inline.hpp:2233` | 删除 TypeInfo 范围诊断计数和日志；保留原对象尺寸检查 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_TRACE_CLEAR` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_UNTAG_BREADCRUMB` | 编译宏 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_VERIFY_HEAP` | 真实getenv（字面） | 冻结 runtime/src 无此词法项 | 冻结前已迁移正式 ZVerifyObjects；本轮不改该实现 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:81` |
| `MRT_GCV2_VERIFY_MARKING` | 真实getenv（字面） | 冻结 runtime/src 无此词法项 | 冻结前已迁移正式 ZVerifyMarking；本轮不改该实现 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:84` |
| `MRT_GCV2_VERIFY_OBJECTS` | 真实getenv（字面） | 冻结 runtime/src 无此词法项 | 冻结前已迁移正式 ZVerifyObjects；本轮不改该实现 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:81` |
| `MRT_GCV2_VERIFY_OOPS` | 真实getenv（字面） | 冻结 runtime/src 无此词法项 | 冻结前已迁移正式 ZVerifyOops；本轮不改该实现 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:119` |
| `MRT_GCV2_VERIFY_PAGE_OWNER` | 真实getenv（字面） | `runtime/src/Heap/Allocator/RegionInfo.h:51` | 删除 count-only 分支及计数报告；保留默认 owner 断言 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:119` |
| `MRT_GCV2_VERIFY_POST_EVAC` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_VERIFY_REGIONS` | 真实getenv（字面） | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_VERIFY_ROOTS` | 真实getenv（字面） | 冻结 runtime/src 无此词法项 | 冻结前已迁移正式 ZVerifyRoots；本轮不改该实现 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:78` |
| `MRT_GCV2_VERIFY_STACK_ROOTS_COMPLETE` | 真实getenv（字面） | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_WAITFWD` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_WHOZERO` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_ZAP_ALLOC` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_ZAP_RECLAIM` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_ZGC_SELFHEAL` | 注释/日志名称残留 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_ZGC_SELFHEAL_ABORT` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |
| `MRT_GCV2_ZGC_SELFHEAL_REPORT` | pinned/编译期钉死 | 冻结 runtime/src 无此词法项 | 删除旧名（冻结前已无该词法项）；本轮核对完整清单，不认领前置机制删除 | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/z_globals.hpp:30` |

### 旧清单外的测试残留

- `MRT_GCV2_CONCURRENT_STACK_SCAN`：`runtime/tests/gc_unit/test_young_conc.cpp:1469`、`runtime/tests/gc_unit/test_young_conc.cpp:1471`；删除旧环境设置/归档说明中的名称，无新替代开关。
- `MRT_GCV2_DISABLE_MINOR`：`runtime/tests/gcparity/run_one.sh:58`；删除旧环境设置/归档说明中的名称，无新替代开关。
- `MRT_GCV2_FULL_YOUNG_SCAN`：`runtime/tests/gcparity/README.md:17`、`runtime/tests/gcparity/run_one.sh:50`；删除旧环境设置/归档说明中的名称，无新替代开关。
- `MRT_GCV2_HAPILLAR_CENSUS`：`runtime/tests/perf_vs_official/analyze_hapillar.py:6`；删除旧环境设置/归档说明中的名称，无新替代开关。
- `MRT_GCV2_MARKPAR_FORCE_SERIAL`：`runtime/tests/gc_unit/remap_window_fixture.hpp:341`、`runtime/tests/gc_unit/test_young_conc.cpp:518`、`runtime/tests/gc_unit/test_young_conc.cpp:557`、`runtime/tests/gc_unit/test_young_conc.cpp:636`、`runtime/tests/gc_unit/test_young_conc.cpp:734`、`runtime/tests/gc_unit/test_young_conc.cpp:792`、`runtime/tests/gc_unit/test_young_conc.cpp:845`、`runtime/tests/gc_unit/test_young_conc.cpp:899`、`runtime/tests/gc_unit/test_young_conc.cpp:959`、`runtime/tests/gc_unit/test_young_conc.cpp:1006`、`runtime/tests/gc_unit/test_young_weak.cpp:696`、`runtime/tests/gcparity/run_one.sh:51`；删除旧环境设置/归档说明中的名称，无新替代开关。
- `MRT_GCV2_YOUNG_CONC_FOLLOW`：`runtime/tests/gc_unit/test_young_conc.cpp:1462`、`runtime/tests/gc_unit/test_young_conc.cpp:1464`；删除旧环境设置/归档说明中的名称，无新替代开关。
- `MRT_GCV2_YOUNG_CONC_MARK`：`runtime/tests/gc_unit/test_young_conc.cpp:1455`、`runtime/tests/gc_unit/test_young_conc.cpp:1457`；删除旧环境设置/归档说明中的名称，无新替代开关。

## 词法差集与阳性核

同一 `git grep` 尺扫描两棵树，原始 stdout/rc 位于本棒 `d07_evidence/scan-commands.txt`。候选 `git grep -n MRT_GCV2 <候选> -- runtime/src runtime/tests` stdout 为空，rc=1；冻结臂有真实命中，rc=0。使用 `-c` 的分目录命令亦保留原文，不把 rc=1 当程序失败或把无输出写成打印了数字 0。

| 保留符号（同尺 git grep -c） | 冻结 | 候选 |
|---|---:|---:|
| `ZVerify::BeforeZOperation` | 11 | 11 |
| `ZVerify::AfterMark` | 2 | 2 |
| `ZVerify::BeforeRelocation` | 2 | 2 |
| `ZVerify::AfterRelocation` | 3 | 3 |
| `MarkPartialArray::FollowElements` | 1 | 1 |
| `ZRelocationSetSelector` | 4 | 4 |
| `FORWARDING_FACE_RESET_BIT` | 5 | 5 |
| `StampRetainedSnapshot` | 3 | 3 |
| `PlausibleManagedObjectGate(` | 60 | 60 |
| `TryRecoverInteriorBase` | 26 | 26 |

这些是源码存在性/调用字符串检查，不能外推运行结果。`ZVerify` 与 `ZRelocationSetSelector` 覆盖近期 D06a/D06b 主线包的特征；保留字段面、选择器与数组核心调用。

## 测试同批清理

- `PartialArray` 与 `MarkPort203Entries` 既有数组用例保留，仅删除旧开关 unset；功能锚是 ZMark 的数组分块函数。未找到同名 ZGC 分块专用测试，不把我方测试冒称上游移植。
- `StackWatermark.MarkCompletionDoesNotCompleteRemap`、`RemapRetainsLogicalStackIdentityAcrossGrow` 的产品断言保留；删除仅服务自有开关/注入器的两个 harness 与两个 probe 脚本。ZGC 无这些注入器。
- 四个 Young HEU 延后测试随已删除机制退出，按首轮 advisor 授权。
- 三个 young 必需谓词测试删除 setenv/unsetenv 并改名，产品断言保留。
- 删除已无有效开关臂的 gcparity runner；保留负载与历史分析器，README 标为归档契约。
- `runtime/handshake/HandshakeTimeoutTest.java` 测试 HotSpot 可配置超时；本轮按裁定仅镜像不可配置常量，未移植该 Java 测试，不声称超时路径动态验证。

机械 `GC_TEST` 名称集合差（与独立脚本删除清单分开）：
- 删除/改名前：`runtime/tests/gc_unit/test_defect_regressions.cpp:DefectRegress.YoungFinishDefersHeuAtMostOncePerMajor`
- 删除/改名前：`runtime/tests/gc_unit/test_defect_regressions.cpp:DefectRegress.YoungFinishDoesNotDeferMajorUnderOldPressure`
- 删除/改名前：`runtime/tests/gc_unit/test_defect_regressions.cpp:DefectRegress.YoungFinishInsideExistingWindowDoesNotExtendHeuThrottle`
- 删除/改名前：`runtime/tests/gc_unit/test_defect_regressions.cpp:DefectRegress.YoungHeuDeferralKillSwitchDoesNotTouchSharedClockOrCredit`
- 删除/改名前：`runtime/tests/gc_unit/test_young_conc.cpp:YoungConc.LegacyFollowEnvCannotDisableRequiredEpochHandshake`
- 删除/改名前：`runtime/tests/gc_unit/test_young_conc.cpp:YoungConc.LegacyMarkEnvCannotDisableRequiredEpochHandshake`
- 删除/改名前：`runtime/tests/gc_unit/test_young_conc.cpp:YoungConc.LegacyStackScanEnvCannotDisableRequiredStackScan`
- 改名后：`runtime/tests/gc_unit/test_young_conc.cpp:YoungConc.FollowRequiresEpochHandshake`
- 改名后：`runtime/tests/gc_unit/test_young_conc.cpp:YoungConc.MarkRequiresEpochHandshake`
- 改名后：`runtime/tests/gc_unit/test_young_conc.cpp:YoungConc.StackScanIsRequired`

## 双构型尝试

唯一入口：`/root/cj_build/ops/bin/kkk2_build_two.sh <工作树> sym_cangjie_runtime_493_implement_r5655121433`；脚本 rc=0 不表示构建通过。

| 构型 | CMake 参数 | configure rc | build rc | wall | 产物 |
|---|---|---:|---|---|---|
| default | MRT_TESTABLE_INTERNALS=OFF, Release | 1 | NOT_RUN | 3s | 未到 SO 链接 |
| testable | MRT_TESTABLE_INTERNALS=ON, Release | 1 | NOT_RUN | 3s | 未到 SO 链接 |

两臂并发；kkk2 nproc=192，配方为 `cmake --build … -j$(nproc)`，因 configure 失败，实际构建 -j 未执行。前后 uptime/load、源码 tar 完整 SHA256 见 `d07_evidence/build-metadata.json`。两臂均在 `runtime/src/Heap/z/zPage.inline.hpp:1124` 的既有 `sizeof(UnitInfo) == 216` 静态断言失败：本包删除探针字段后元数据布局变化。本轮不改断言、不加占位字段，不声称可编译；按形态对齐合同原样交付构建结果。没有运行 unit/gate/切刀，没有测试 ELF 或 SO 运行结论。

远端日志：`kkk2:/root/sym_cangjie_runtime_493_implement_r5655121433/{default,testable}-configure.log`。

## FALSIFIED

第二次 advisor 曾把现有 ZVerify::{Before,After}Relocation 对应到 ZPage::verify_live。实读证明前者检查 remset/字段，后者比较精确 live_objects/live_bytes；第三次答复已纠正并将精确对应登记至 D10 #503。

## 第二轮构建修复

本轮起点 `296e365d9641d739f3f035d72eb6b94ff2157011`。删除该版本
`runtime/src/Heap/z/zPage.inline.hpp:1122` 起的固定 216 字节预算断言和两行旧注释。
ZGC `zPage.hpp:45`–`:55` 按实际成员组织页元数据，没有 UnitInfo 固定字节预算对应物。
我方 `zPage.hpp:1160`、`:1168`、`:1252` 按实际 sizeof(UnitInfo) 计算元数据位置；
本修复不补回已删除的探针字段、不增加占位字段或开关。
