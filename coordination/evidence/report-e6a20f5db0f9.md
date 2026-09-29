PROGRESS=WIP · verdict=补齐 ZGC 重载与生命周期后重验中；下文旧 head 结果暂不用于本次交付 ｜尺=同ELF切刀矩阵 目标转红=5 N=31 · LANE=sym_cangjie_runtime_1305_implement_r5892797534
DELIVERY_REF=cangjie-runtime|sym/1305-implement-r5892797534|e6a20f5db0f98b2cf3cd618e5e41dd9e054dc9ff
SIDE_EFFECT: 已推 cjcjdev/sym/1305-implement-r5892797534，已开 cangjie-runtime#1325（cjcj-bot）；未推主线，未开新 issue。
ROLE=implement
EVIDENCE=local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence,kkk2:/root/sym_cangjie_runtime_1305_implement_r5892797534/matrix,kkk2:/root/sym_cangjie_runtime_1305_implement_r5892797534/keep
LANE=sym_cangjie_runtime_1305_implement_r5892797534

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
本轮按 #1305 正文实现。坐标基于 `9733dfc09d29eca27d19cc3937a149838b4e3322`。冻结回读命令 `git -C /root/cj_build/cangjie_runtime rev-parse cjcjdev/main`：rc=0，输出 `9733dfc09d29eca27d19cc3937a149838b4e3322`。
交付前再次 `git fetch cjcjdev && git merge cjcjdev/main`：rc=0，Already up to date；主线仍为冻结 SHA。`git diff --check cjcjdev/main...HEAD` rc=0；冲突标记检索 rc=1（空）。
PR：https://github.com/cjcj-dev/cangjie-runtime/pull/1325，API author=`app/cjcj-bot`、is_bot=true，head=e6a20f5db0f98b2cf3cd618e5e41dd9e054dc9ff。

## CLAIM
CLAIM: 终止、对象集合、唯一访问、数组分块和真实根入口都有产品结果进入断言的可证伪证据。
  METHOD: test
  EVIDENCE: runtime/tests/gc_unit/test_young_weak.cpp:841; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/remote/matrix/results.json; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/remote/matrix/failure-assertions.txt
  N: 31（六构型各五项，加根入口切刀一项；每次独立进程）

CLAIM: 最终候选 default/filler/testable 三臂已实际运行，三臂差分 CAND-ONLY=0。
  METHOD: test
  EVIDENCE: /root/cj_build/reports/DIFF-e6a20f5db0f9-vs-9733dfc09d29.json; kkk2:/root/sym_cangjie_runtime_1305_implement_r5892797534/unit-final-default/run.log; kkk2:/root/sym_cangjie_runtime_1305_implement_r5892797534/unit-final-filler/run.log; kkk2:/root/sym_cangjie_runtime_1305_implement_r5892797534/unit-final-testable/run.log
  N: 3（default 1359、filler 1359、testable 1513 个发现并执行的用例；非负载完成判词）

CLAIM: 旧 map 查询/安装、自有位图 CAS、裸 vector 窃取和单 worker 根路由已被替换，没有开关并存路径。
  METHOD: read
  EVIDENCE: runtime/src/Heap/z/zHeapIterator.cpp:57; runtime/src/Heap/z/zHeapIterator.cpp:254; runtime/src/Heap/z/zHeapIterator.cpp:276; local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/source-evidence.txt

## FALSIFIED
1. **主控正文原转红前提错误**：空闲 worker 提前返回不必导致访问集合遗漏；其余 worker 可完整处理自己的链，全部 join 后集合仍完整。本轮终止刀实测 early_return=1，但 visits=1025 且独立 ReachableSet 仍绿，直接证伪原前提。advisor 已批准改成“仍有工作时其他 worker 不得返回”：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_1305_implement_r5892797534-20260929T145851Z.md:1`。
2. 测试夹具最初误用了 NativeSlot 默认构造。两次编译 rc=123；第一次误判为复制问题，第二次依 `runtime/src/ObjectModel/RefField.h:148-156` 明确为默认构造删除，改为显式 null 初始化。编译失败不计转红。
3. OHOS 首次 configure rc=1：系统 libc.so 是 linker script，loader self-check 报 invalid ELF header。本棒复制 libc.so.6 为本棒 shim/libc.so，按 `runtime/build/cmake/CheckOHOSHostLibraries.cmake:1` 在 configure 时设置 LD_LIBRARY_PATH 后通过。属于**资源装置修正**，原失败日志保留 `kkk2:/root/sym_cangjie_runtime_1305_implement_r5892797534/ohos/configure-first.log`，未修改共享库或守卫。

## producer→consumer（实施前已记录）
实施前表保留在 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/report-wip.md`；下表为最终锚。
| producer | consumer 顺序 | 需要保证的位置 |
|---|---|---|
| RootsIteratorStrongColored::Apply，zRootsIterator.cpp:136 | zHeapIterator.cpp:191 do_root → :88 mark_visit_and_push → :96 context.push | 去重成功后才发布队列；根迭代状态在所有 worker 间共享 |
| zHeapIterator.cpp:128 OopClosure::do_oop | mark_visit_and_push → mark_object → object_bitmap → CHeapBitMap::par_set_bit | 原子 bit 决定唯一入队；get_acquire / lock-recheck / release_put 安装位图 |
| zHeapIterator.cpp:143 follow_array / :148 follow_array_chunk | context.push_array_chunk → drain 或 steal_array_chunk → follow_array_chunk | 分块续项先发布，再处理当前范围 |
| context.pop / pop_array_chunk / 两种 steal | :172 visit_and_follow、:148 follow_array_chunk → :276 drain_and_steal | local drained 后才能 offer，全体 offer 前不得退出 |
| zHeap.cpp:564 Heap::object_iterate / :570 verify | :291 object_iterate / :296 object_and_field_iterate → roots → drain_and_steal | 真实运行时消费者；OverflowRoots 直接进入 Heap::object_iterate |

## ZGC 函数对应表
以下为实现对应记录；最终审查判词由 Review 给出。ZGC 根路径为 `/root/cj_build/reference/jdk/src/hotspot/share/`。
| 我方 | ZGC file:line | 形态 |
|---|---|---|
| HeapIteratorBitMap::try_set_bit，zHeapIterator.cpp:16 | gc/z/zHeapIterator.cpp:42-54 | CHeapBitMap::par_set_bit，删除私有原子 word vector |
| object_index_max / object_index / object_bitmap / mark_object，:45-76 | gc/z/zHeapIterator.cpp:300-347 | granule offset 与对象对齐位移；acquire 查询、锁内复查、release 发布 |
| mark_visit_and_push，:88 | gc/z/zHeapIterator.cpp:421 | mark → verify visitor → push |
| HeapIteratorContext::push/pop/is_drained | gc/z/zHeapIterator.cpp:58-120 | 队列指针；overflow 优先，owner pop 次之 |
| 构造/析构，共享 roots 与 terminator | gc/z/zHeapIterator.cpp:254-297 | 队列集、数组队列集、三类根迭代器、terminator；回收 granule bitmaps |
| push_strong_roots / push_weak_roots / push_roots，:207-236 | gc/z/zHeapIterator.cpp:391-419 | 各 worker 进入同一共享根状态；删除 worker_id==0 分路 |
| drain / steal 三分解，:239-273 | gc/z/zHeapIterator.cpp:480-514 | 数组优先窃取，偷到后直接 follow；对象其次 |
| drain_and_steal / object_iterate_inner，:276-289 | gc/z/zHeapIterator.cpp:517-530 | 同一循环条件调用 offer_termination |
| TaskQueueSuper / GenericTaskQueue / OverflowTaskQueue | gc/shared/taskqueue.hpp:153,333,437 | native-width tagged age、bottom、固定环、owner overflow；LP64 2^17 槽 |
| GenericTaskQueueSet::steal_best_of_2 / steal | gc/shared/taskqueue.inline.hpp:315-391 | 上次 victim、best-of-two、2-worker 分路、2*n 次重试 |
| TaskTerminator::DelayContext / offer_termination | gc/shared/taskTerminator.cpp:39-218 | offered count、spin master、解锁查任务、定时等待、全体退出 |
| ZGranuleMapIterator，zGranuleMap.hpp:86 | gc/z/zGranuleMap.inline.hpp:117 | 复用 ZArrayIteratorImpl |

基础设施事实：HotSpot CHeap/Stack/队列指针数组存储由 C++ unique_ptr/vector 承载（taskqueue.hpp:405,440,487），owner/steal 分路不变；位图已直接复用现有 CHeapBitMap，无需位图存储例外。Cangjie 栈/无头记录扫描继续通过既有 Mutator::VisitMutatorRoots/VisitHeapRootSlots 进入 uncolored closure，对应 ZGC zHeapIterator.cpp:382-410 的线程闭包。

## 删除清单
命令模板 `git grep -F -c '<项>' <ref> -- runtime/src`，完整命令、输出、rc 在 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/source-evidence.txt`。每项基线 rc=0、HEAD rc=1；这是同一范围的差集，不是只报空命中。
| 被删除的旧实现文本/符号 | 基线命中 | HEAD 命中 |
|---|---:|---:|
| `objectBitmaps.find` | 1 | 0 |
| `objectBitmaps.emplace` | 1 | 0 |
| `HeapIterator::try_set_bit` | 1 | 0 |
| `HeapIterator::Push` | 1 | 0 |
| `workerQueues.resize` | 1 | 0 |
| `context.queue.push_back` | 1 | 0 |
| `std::vector<std::vector<BaseObject*>>` | 1 | 0 |
| `words[index / bits]` | 1 | 0 |
HeapIterator::try_set_bit 并非单纯改名：原 map+mutex 整段已删除，职责分成 object_bitmap 与 mark_object；CHeapBitMap 替换旧 words CAS；原 vector 直接跨 worker pop 被完整删除。

## 产品接线证明
共同身份：`nm --defined-only` 在测试 ELF 找不到 HeapIterator 产品实现（grep rc=1），同一命令找到 P82 测试函数作为阳性对照；`nm -u` 有产品构造、析构、object_iterate、object_and_field_iterate 的版本化导入；SO 完整符号表有对应定义。证据：`local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/remote/matrix/{test-symbols,product-imports,product-copies,product-copies.grep-rc}` 与 `remote/arms/*/product-symbols.txt`。
| 测试名（P82HeapIterator. 前缀） | 产品函数 | 断开会红吗 | 产品结果与目标 |
|---|---|---|---|
| TerminationAgreement | zHeapIterator.cpp:276 → zTaskTerminator.hpp:64 | 是，仅此项 | visitor 持有对象期间另一 worker 是否从公共迭代入口返回；:872 的断言 |
| ReachableSet | zHeapIterator.cpp:254 steal → visit_and_follow | 是，仅此项 | 产品 visitor 返回对象集合，与输入可达集合比较；:890 |
| VisitsOnce | zHeapIterator.cpp:72 mark_object → Base/BitMap.h:177 | 是，仅此项 | 每对象实际访问次数，重复根与跨 granule 汇合；:908 |
| ArrayChunks | zHeapIterator.cpp:254 → follow_array_chunk | 是，仅此项 | 产品 fieldVisitor 返回的全部 4097 个槽，每槽恰好一次；:971 |
| OverflowRoots | zHeap.cpp:564 → zRootsIterator.cpp:136 Apply → mark/push/pop | 是 | Heap::object_iterate 返回集合；LP64 容量外的 132000 个根；:1004 |

逐面闭环自评：**✅ 证据闭环成立**（实施证据自评，不是 Review 放行）。①同一已链接产品入口；②返回状态、对象或槽进入断言并打印 `P82_*_ASSERT`；③产品切刀导致对应目标断言失败，恢复回绿。没有重编同名产品副本、手工向队列/位图注入结果、测试钩子、interposition 或入口无条件报错。

## 断线臂与精确转红
全部运行在 kkk2、ulimit -c 0、核域 **48-63**（cjops windows 分配）；矩阵并行 31 个独立进程，同一 ELF：
`97d5b8292722a7e34a1e872a154e6add96598912253922711626f43d242e0416`。
运行脚本 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/run-cut-matrix.sh`，命令入口 `bash /root/cj_build/tools/box.sh kkk2 'ulimit -c 0; bash /root/sym_cangjie_runtime_1305_implement_r5892797534/run-cut-matrix.sh'`。
两端 uptime：23:26:25 / 23:26:28，均 up 19 days 23:34；load average 均 38.62,85.16,78.16。原文 `remote/matrix/uptime-{before,after}.txt`。每项 wall 单独保存；矩阵整体约 3s，不作性能判词。

| 切刀（产品行） | 绿 rc/通过数 | 红 rc/通过数 | 恢复 rc/通过数 | 精确目标观测 |
|---|---|---|---|---|
| termination，zHeapIterator.cpp:281，去掉 offer | 0 / 5 | 1 / 4 | 0 / 5 | early_return 0→1→0；独立集合断言保持绿 |
| objects，zHeapIterator.cpp:261，断偷到对象后的消费 | 0 / 5 | 1 / 4 | 0 / 5 | ReachableSet exact 1→0→1 |
| arrays，zHeapIterator.cpp:259，断偷到分块后的消费 | 0 / 5 | 1 / 4 | 0 / 5 | ArrayChunks exact 1→0→1 |
| bitmap，Base/BitMap.h:187，把已置 bit 报成新置位 | 0 / 5 | 1 / 4 | 0 / 5 | VisitsOnce once 1→0→1；集合仍完整 |
| entry，zRootsIterator.cpp:141，断 statics.apply | 0 / 1 | 1 / 0 | 0 / 1 | OverflowRoots seen 132000→0→132000 |

每项 rc 指对应选定组中的失败状态；原始逐进程 rc 在 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/remote/matrix/results.json`。entry 仅跑真实 Heap 入口的 OverflowRoots，绿/恢复复用同矩阵该项；其余四刀均跑同一五项，不存在一刀全红。所有失败均有目标打印和该行 assertion failure，没有更早致命断言遮蔽。

切刀补丁：`local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/{termination,objects,arrays,bitmap,entry}.diff`；构建日志与 rc：`kkk2:/root/sym_cangjie_runtime_1305_implement_r5892797534/arms/<arm>/{configure,build}.{log,rc}`；红/恢复日志：`kkk2:/root/sym_cangjie_runtime_1305_implement_r5892797534/matrix/<arm>/<test>.log`（均已拉回本地 remote/）。
基线刀 entry+bitmap 的 `entry_cut_check.json` **rc=0**，命中已登记 `runtime/src/Heap/z/zRootsIterator.cpp:Apply`。两处共享文件仅在隔离副本切刀：候选/恢复源码 cmp rc=0；本地前后 sha256 相同、git status 空；见 `remote/matrix/source-restore.txt` 和 `transient-{before,after}.sha256`。

### 候选新增行刀（按常备裁决 3）
termination :281、objects :261、arrays :259 均绑定 @e6a20f5db0f98b2cf3cd618e5e41dd9e054dc9ff；新终止接线和新 steal 消费路径只能在候选新增行切断，不冒充冻结基线刀。其独立 checker 原始 rc=1 保存在 `candidate-line-cuts.{json,log}`；基线 entry+bitmap 的 rc=0 另行提供。三刀均使用上述同一测试 ELF，下列 SO 身份满足要求，且目标分别转红。

| 产品构型 | SO sha256 |
|---|---|
| 候选 | `79e19643e68257055d9faafc3a8c23f9e23f3fece421621118a9276bb9fb4cc6` |
| 恢复 | `79e19643e68257055d9faafc3a8c23f9e23f3fece421621118a9276bb9fb4cc6` |
| 切刀终止 | `a976e4b2de842ce6013b47056b6f19509e2ac5ab12674b68b99bfac491dddff2` |
| 切刀对象窃取 | `2c4d10767bc3ecbd3172bd0512a5e4fc4dcb93dfa2f4843bf8f1a7d1310e02a1` |
| 切刀数组窃取 | `bef232064a213f04e780659bae08ed649af486401a9be30d42c45688dcb85a80` |
| 切刀位图 | `ca82e8f19e7f5fcf08d8b52f33a053482993b91a69ae9214f09a53a57135b030` |
| 切刀根入口 | `6ba2f059f30785563aa842576466ef252fb73076bf4b8397dfd97af6973899a3` |
所有臂 boundscheck 同为 `c1205ffc1878a3d3663bdf78089ff3a07746ee6f2562d96e5fe28ed34ed5272a`，trace 同为 `5e25ea529f9a963956fe0c814058b4ddd5ab741bd39693b0ae73e406ffaab2b5`。运行 LD_LIBRARY_PATH 指向 `/root/sodepot/<该SOsha>/`，三个 SO 均实体 cp。
候选 lineage：`CJRT-COMMIT:src-97b993eda04f4eef43aec2b4968e8562bff1187de72ad49a94bc408f9ad07783`、`CJRT-DECLARED:e6a20f5db0f98b2cf3cd618e5e41dd9e054dc9ff`。各臂链接后立即保存哈希并 cp 到 keep，测试使用保留副本；构建时刻与源码时刻在 `remote/arms/*/timestamps.txt`。同配方、同产品输入的候选/恢复是字节相同的阳性身份对照。

## 承重面清单
机械导出命令和完整命中整行见 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/consumers.txt`：从 mark_visit_and_push / follow_array_chunk / offer_termination 直接消费者向上到 Heap::object_iterate、验证与堆转储路径；未把字符串当调用点。
| 消费者/分支 | 产品锚 | 臂 | rc/证据 |
|---|---|---|---|
| 单/多 worker（1/2/4） | zHeapIterator.cpp:291 | ReachableSet、VisitsOnce，各 12 个组合 | 绿/恢复 0，目标刀 1，matrix 对应日志 |
| 强/含弱根、mark/follow visitor 时机 | zHeapIterator.cpp:172,232,301 | 同上；原有 HeapIterator 四项仍保留 | 最终 testable 全套 1513/1513 |
| 普通对象 owner pop / foreign steal | zHeapIterator.cpp:106,254 | ReachableSet | objects 1，其余四项 0 |
| 数组当前 chunk / continuation / foreign steal | zHeapIterator.cpp:148,239,254 | ArrayChunks，weak=false/true | arrays 1；candidate/restored 0 |
| 有工作 / 全体空闲退出 | zHeapIterator.cpp:276 | TerminationAgreement | termination 1；candidate/restored 0 |
| 位图已装 / 首次安装、已置 / 新 bit、跨 granule | zHeapIterator.cpp:57,70 | ReachableSet、VisitsOnce、OverflowRoots | 集合完整；bitmap 仅唯一访问 1 |
| 有界环 / overflow，公共 Heap 入口 | zHeap.cpp:564；zHeapIterator.cpp:106 | OverflowRoots（132000 根） | entry 1；candidate/restored 0 |
| 原有强弱引用与 field 遍历、verify 入口 | zHeap.cpp:570；zHeapIterator.cpp:296 | 全套既有用例 + 新图 verify 组合 | testable/default 全套结果见下，不声称每个 GC 相位都另有独立刀 |

## 测试增删
相对冻结基线新增五项：P82HeapIterator.TerminationAgreement、ReachableSet、VisitsOnce、ArrayChunks、OverflowRoots，位于 test_young_weak.cpp:841,875,893,911,974。删除=0、改名=0、弱化既有断言=0。测试名集合差在 DIFF 中为 testable 1508→1513；default/filler 同为 1359。新增用例是 ZGC 产品算法的定向输入构造，不是复制产品算法的模型测试。

## 双构型证明及单元结果
两构型入口仅使用 `/root/cj_build/ops/bin/kkk2_build_two.sh <本工作树> sym_cangjie_runtime_1305_implement_r5892797534`；default/testable 独立目录并行，各 -j48（当前 helper 固定配方），wall=19s/16s。切刀单构型七臂并行，每臂 cmake --build -j192；wall=23–38s。OHOS 独立 configure/build -j192。所有编译带 ccache 和三项 prefix-map。
| 构型 | CMake 变量 | configure/build rc | 产品 SO sha256 | 用例状态 |
|---|---|---|---|---|
| default | -DMRT_TESTABLE_INTERNALS=OFF | 0/0 | 842efee5858a4be5c108265fb674850822bb27000c563724e714ca29902244cd | 1359 passed |
| testable | -DMRT_TESTABLE_INTERNALS=ON | 0/0 | 79e19643e68257055d9faafc3a8c23f9e23f3fece421621118a9276bb9fb4cc6 | 1513 passed，含本包五项 |
| OHOS-host | -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON | 0/0（首次 configure=1 已保留） | 4418a8c7504d2991e08953dedb59190dedbc1f58b693c4390777dd1727e153ce | 六项 passed |
实现本体没有 MRT_TESTABLE_INTERNALS 分支、新测试 friend 或导出。宏开构型用于现有内部访问装置；默认构型编译同一实现，本包定向结果不伪称在 default 中跑过。

UNIT_DEFAULT_RC=0
UNIT_FILLER_RC=0
UNIT_OHOS_RC=0
UNIT_TESTABLE_RC=0
日志分别 `kkk2:/root/sym_cangjie_runtime_1305_implement_r5892797534/unit-final-default/run.log`、`unit-final-filler/run.log`、`ohos/run.log`、`unit-final-testable/run.log`（后面三条均以同一远端根路径补全）。filler 由 **同一 default ELF** 设置 CJRT_HEAP_FILLER=0 后运行，ELF sha256 文件相同；testable 不是 filler。
完整 CMakeCache、SO/ELF 哈希、rc、wall 均在 `local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/remote/`。全套测试 jobs=192，default/testable 并行；filler 必须等 default ELF 产生后复用，故此依赖部分顺序。测试 wall（含编译）default=164s、testable=168s，filler=35s；无顺序步骤超过 10min。
单元前后 uptime 23:22:36 / 23:25:55，load 113.47,112.08,81.36 → 45.02,91.16,79.80；原文 `remote/units-uptime-{before,after}.txt`。全套使用未钉核 192 核；定向矩阵使用 48-63。

## 三臂差分与 managed 限制
`/root/cj_build/reports/DIFF-e6a20f5db0f9-vs-9733dfc09d29.{json,md}`：default/filler/testable status=ran，两端 rc=0，各 CAND-ONLY=0；没有删除或调整失败期望。基线命中差分服务共享缓存，未私建基线。
managed status=NOT_RUN，不计通过。两端 N=3 的 phase 编译同报 `undefined reference to CJ_MRT_RequestStringDedup`，SDK=`/root/sdkdepot/b99430a618af-1ecb811801ca`；签名锚为 `/root/diff_{e6a20f5db0f9,9733dfc09d29}/managed-runs/stained_phase_n1/phase_entry_trigger.build.log:54`。按 0926 11:4x 第二签名常备例外允许送 Review，不改 SDK/runner、不伪造四臂通过。

## 主线内容保留与资源归置
`local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1305_implement_r5892797534/coordination/evidence/main-preservation.txt` 给主线近期 #1286 包的 git grep -F -c 对照：ThreadGCData::InstallMasks 与 ZBarrierSet::on_thread_attach、ThreadsListHandle 的 HEAD 计数均不低于 main；相应 zBarrierSet.cpp/zThreadLocalData.cpp/zMark.cpp/Mutator.cpp/ThreadLocal.cpp 内容 diff rc=0。并保留首次错误符号检索 rc=1，未用其证明机制存在。
六个变更文件只涉及本包产品与测试；无主线提交、无共享 SDK 改动。构建树、源码副本和未保留测试 ELF 已删除；远端最终目录约381M，只保留日志、证据和 keep/。本地未新建 /root 顶层目录，未使用 /tmp 产物。

## §5 角色表
| 角色 | 本轮产出 | 边界 |
|---|---|---|
| implement | 产品提交、测试、五刀及恢复、候选分支与 PR | 未作 Review 放行，未合并主线 |

## 形式检查
lane_selfcheck rc=1，唯一 FAIL 是 managed=NOT_RUN；其余头、远端 head、三臂差分与 SO 身份检查通过。按 0926 11:4x StringDedup 第二签名常备例外和 managed 自检例外送 Review；managed 不计通过。原样输出保留在 coordination/evidence/lane_selfcheck.log：

```
PASS [4-header] 首行以 PROGRESS= 开头且含 LANE=
PASS [1-delivery-ref] cjcjdev/refs/heads/sym/1305-implement-r5892797534 == e6a20f5db0f98b2cf3cd618e5e41dd9e054dc9ff
INFO [2-diff] default: CAND-ONLY=0
INFO [2-diff] filler: CAND-ONLY=0
INFO [2-diff] testable: CAND-ONLY=0
FAIL [2-diff] DIFF-e6a20f5db0f9-vs-9733dfc09d29.json 有臂未 ran: managed='NOT_RUN'
PASS [3-so-hash] 候选≠切刀、候选∩恢复≠∅（候选 2 / 恢复 2 / 切刀 5 枚，启发式）
SUMMARY fail=1 warn=0 rc=1
```

`cjops deliver check --lane sym_cangjie_runtime_1305_implement_r5892797534` rc=0：**形式检查通过；实质由 Review 判**。`entry_cut_check` 基线刀 rc=0；新增行刀 rc=1 已按常备裁决3单列。差分服务 wrapper rc=3（managed NOT_RUN），不冒充四臂通过。

SYM-PR: cangjie-runtime#1325
SYM-NEXT: stage=Review

新增 head=355b521a5de5933bec504f0a1a44b7503e849f05，旧 e6a20f5db0 验收保留为历史，新 head 正在重跑。
