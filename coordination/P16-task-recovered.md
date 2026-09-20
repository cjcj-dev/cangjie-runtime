LANE=sym_cangjie_runtime_627_implement_r5744767112
ROLE=implement

# cangjie-runtime#627 · Implement · 形态对齐 · P16 · zVerify 形态对齐 + 全树诊断/测试钩子/旧目录清扫

## ⭐ 本轮的身份与边界（⛔ 逐条都是硬约束）

- 看板条目：cangjie-runtime#627 https://github.com/cjcj-dev/cangjie-runtime/issues/627
- 档：**Implement**（角色 `implement`）· 第 1 次派发
- 归属（Blocker 字段）：⛔ 未填
- 工作目录：`/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_627_implement_r5744767112`（隔离工作树，分支 `sym/627-implement-r5744767112`）
- 墙钟上限：**180 分钟**

## ⭐⭐⭐ 交付路径（⛔ 只此一个，⛔ 别改名）

```
/root/cj_build/reports/REPORT-sym_cangjie_runtime_627_implement_r5744767112.md
```

⭐ 首行三态：`PROGRESS=WIP` ＝ 还在做（派发器会续跑）· `TRIAGED` / `DONE` ＝ 停。
⛔ **还在做就写 `WIP`** —— 写 `TRIAGED` 会让派发器停你（曾害一条棒空停 6 小时）。
⭐⭐ **边做边写**，且**每个可编译中间态就 commit** —— `reports/` 与工作树都可能被下一会话清掉。

报文头逐字如下（⛔ 三行都要）：
```
LANE=sym_cangjie_runtime_627_implement_r5744767112
ROLE=implement
PROGRESS=WIP
```

## ⭐⭐ 冻结坐标（⛔ 必须自己回读一遍并记 rc，⛔ 不许从别处推）

| 仓 | **绝对目录**（⛔ 只认这个） | ref | 40 位 sha |
|---|---|---|---|
| cangjie-runtime | `/root/cj_build/cangjie_runtime` | cjcjdev/main | `325d6ad73f99cc463a804aedd39595af05791c9a` |

⛔⛔ **本机存在同名的其它检出** —— ⭐ 只认上表那个目录，⛔ 别按仓名去 `find`。
⭐ 例：`cjcj-llvm` 在本机有三处，三处的 `origin/main` **真的不同**（⭐ 其中 `llvm_src/cjcj-llvm` 是 Q110 登记的**第二只读分析坐标**，⛔ 不是规格锚）。

⭐ 回读命令（⭐ rc 与文本**分开取** —— `cmd | tail` 后读 `$?` 拿到的是 tail 的）：
```bash
git -C /root/cj_build/cangjie_runtime rev-parse cjcjdev/main
```

## ⭐⭐⭐ 分支与主线（0913 用户令）

- 本工作树分支 `sym/627-implement-r5744767112` 是本条目的**候选分支**：返工轮直接在它上面继续提交、推同一分支、PR 复用；⛔ 不 cherry-pick「承接前轮」、⛔ 不另起分支。
- ⭐⭐ **交付前**先 `git fetch cjcjdev && git merge cjcjdev/main`（主线在你开工后很可能已前进）：逐文件解冲突，**两侧机制都保留**（主线刚合入的包与你的包），⛔ 不抹任何一方；解完再两构型构建、再交付。
- 报告里给 `git grep -c` 证明：主线近期合入包的特征符号在你的 head 上计数不少于 `cjcjdev/main`；审查用 `git diff cjcjdev/main...<head>` 看本包净差分。
- 沙箱若拒绝 `git push`：交 DONE 并在报告写明，主控代推；⛔ 不要为此停在 TRIAGED。

## ⭐ 本条 issue 的正文（⭐ 唯一任务来源）

Blocked-by: cangjie-runtime#648
Blocked-by: cangjie-runtime#577
Created-by: 主控（0915，形态普查 Workflow wf_93e21d75-621 → reports/ZGC-SHAPE-ALIGNMENT-0915/PLAN.md §4 P16）
Domain: ① correctness
Blocked-by: cangjie-runtime#608
Blocked-by: cangjie-runtime#609
Blocked-by: cangjie-runtime#610
Blocked-by: cangjie-runtime#611
Blocked-by: cangjie-runtime#612
Blocked-by: cangjie-runtime#613
Blocked-by: cangjie-runtime#614
Blocked-by: cangjie-runtime#615
Blocked-by: cangjie-runtime#616
Blocked-by: cangjie-runtime#617
Blocked-by: cangjie-runtime#620
Blocked-by: cangjie-runtime#621
Blocked-by: cangjie-runtime#622
Blocked-by: cangjie-runtime#625
Blocked-by: cangjie-runtime#700
Blocked-by: cangjie-runtime#626

## 主控派发说明（用户令「不要等价，要形态一致」「先形态对齐再修 bug」）
⭐ GPT-6 裁决（reports/CONSULT-gpt6-shape-plan-directions-0915.md）：D4=A；全树 receipt/hook/atexit 清扫 + 旧目录整删 + 门/脚本旧路径同批改（run_*_arms.py、run_standalone.sh、exports.def）。
⭐ 判 ✅ 的唯一标准＝与 ZGC 同一分路点/函数分解/数据形态（只允许名字不同）；功能等价但结构不同 ⇒ (a) 按 ZGC 形态改；基础设施差异只允许 PLAN §5 表内事实，须写事实 + ZGC 锚。⛔ 不放宽 CHECK；⛔ 旧路径加开关并存直接打回；同一分支/PR 返工。
⭐ 基线＝派发时 cjcjdev/main（先回读）；与在飞/已合入条目边界见 PLAN §3（#577/#596/#603/#606/#607 拥有的分路点不重做）。

## 每包统一交付要求（PLAN §7）
1. 交付带「ZGC 函数对应表 + 删除清单」（0912 令）；旧路径加开关并存直接打回。
2. 红臂三条自查：断承重点必转红；红在目标断言上（前置存在性断言与目标不变量断言分开）；单独 filter 跑目标断言证明执行到。
3. kkk2 两构型 -j$(nproc) 并行建，gc_unit default/testable + 托管 runner（finalizer_trigger/segmented_array_managed 等）N≥3；同批移植列出的 ZGC gtest。
4. 编译器同批包（P01/P05/P08）须给 llvm_rebase 侧 PR 与联合 bundle 的门记录。
5. 有前置的包按「Status=Implement + Blocked-by:」建；多 PR 包（P11/P14）每 PR 单独过门、返工同分支。

## 包内容（PLAN §4 原文）
### P16 · zVerify 形态对齐 + 全树诊断/测试钩子/旧目录清扫

- 层级：6 · 来源组：verify-diagnostics、legacy-residue、mark、roots-stack-iterator、relocate-forwarding、remembered-set、pages-object-allocation、references-weak · 依赖：P01、P02、P03、P04、P05、P06、P07、P08、P09、P10、P11、P12、P13、P14、P15 · 可并行：P15
- 风险：低正确性风险，高改动面；门脚本与 exports.def 同批，否则 kkk2 门红

**涉及行（zgc_unit）**：z_verify_safepoints_are_blocked（missing）；z_verify_oop_object/root_oop_object（shape_diff，文件内静态）；z_verify_old_oop 独立；z_verify_possibly_weak_oop 独立；zverify_broken_object 打印+guarantee 回 after_mark；ZVerify::threads_start_processing（P10）；ZVerify::objects（abort 最前、经 ZHeap 包装）；before_zoperation 调用层单点（P14 VM_ZOperation）；after_mark guarantee；after_weak_processing 调用层 pause_verify（P14）；ZVerify::before_relocation 经 ZPage::verify_remset_cleared_*；in-place 重置点 remset 清空检查（missing）；after_relocation_internal 按表遍历+remap_object；after_relocation to_age 入口守卫；ZVerifyForwarding 单调用点；ZForwarding::verify()（P11）；ZPage::verify_live（P02/P03）；ZMark::verify_all_stacks_empty 成员+mark start 调；verify_worker_stacks_empty 分路点；ZStoreBarrierBuffer::is_in 静态+remap（P08）；ZHeap::object_and_field_iterate_for_verify（P14）；[extra] Heap/Verify 独立静态库；[extra] MarkingStacks::VerifyEmpty 17 处；[extra] mark-terminate 计数/atexit/receipt；[extra] WeakDiscoveryTestReceipt；[extra] Y2yHandoff receipt ×11；[extra] ExportRootPublication receipt ×7；[extra] RemsetFilterTestReceipt；[extra] RemapYoungRootsTestReceipt；[extra] ExportOwnershipTestObservation；[extra] MarkClosureObserver；[extra] MarkStripeStack::StorageObserver；[extra] StoreBarrierBuffer 测试面；[extra] AllocBuffer hooks；[extra] WaitEnterHook；[extra] GhostLookupTestHook；[extra] MRT_ALLOCATION_STALL_OBSERVE；[extra] SetBeforeWeakCleanCasForTest；[extra] FinalizerProcessor ForTest 四件；[extra] SetCycleRefHandlerForTest；[extra] FlipTouchCounts；[extra] RootCountForTesting/SetHeapStartForTesting；[extra] ArenaForTest/contains_for_test/ResetForTest/ClearLiveInfo 实例化/ForwardTask 导出；[extra] TracingCollector std::function 钩子；[extra] Driver 测试钩子/GCDriverPortTestPeer；[extra] NoteLargeArrayInit*/LargeArrayInitTestHooks；[extra] NoteEnrolPhase [ENROLTIME]；[extra] g_minorRefCas*；[extra] g_installDomain*；[extra] markwater g_armed/g_turned；[extra] g_markStripeArmed/Turned；[extra] g_invalidMinorRootPrinted；[extra] zombie/from-garbage-skip/fwd-unmarked-keep/fwd-to-gate LOG；[extra] g_forwardRace*；[extra] colourwho；[extra] ForwardingTable 查表计数；[extra] EmitNeverInstalledDiagnostic 死码；[extra] SkippedStackMapCounts 族；[extra] RemsetScanStats；[extra] g_gcTrigger*；[extra] PhaseColourContract.h；[extra] ZVerify::Object 公开被 zForwarding::verify 调；[extra] Heap/CMakeLists 子目录库拆分（legacy 组 UNVERIFIED，见待核）；[extra] MRT_TESTABLE_INTERNALS/MRT_GC_UNIT_TESTS 产品码内分支总清

**形态改动（附 ZGC 锚）**：
- zVerify 按 zVerify.cpp 形态：z_verify_safepoints_are_blocked debug 检查（GC/worker 线程通过；mutator 须不在 saferegion 或 world stopped）挂在 ZBarrier 漏斗/zIterator/ZUncoloredRoot 三处（zVerify.cpp:63-110；调用 zBarrier.inline.hpp:320、zIterator.inline.hpp:43、zUncoloredRoot.inline.hpp:36）；z_verify_oop_object/root_oop_object 文件内静态（:121-129，is_oop=页表有页+地址<top，不再读 TypeInfo）；z_verify_old_oop/possibly_weak_oop 拆回独立、页级 is_allocating、load barrier 不自愈（:131-199）；zverify_broken_object 打印死对象/from 对象、guarantee 回 after_mark（:395-424,503）；objects 顺序 should_abort→roots→threads_start_processing→经 ZHeap::object_and_field_iterate_for_verify（:467-487）；before_zoperation 只在 VM_ZOperation::doit 一处；pause_verify 在 reset 后持 DriverLocker（P14）；before_relocation 经 ZPage::verify_remset_cleared_*（:610-634），in-place 重置点补同一检查（zRelocate.cpp:877-886）；after_relocation_internal address_unsafe_iterate_via_table+remap_object（:724-739），入口 to_age()!=old 守卫（:741-761）；ZVerifyForwarding 单调用点 do_forwarding 后（zRelocate.cpp:1005-1008）；verify_all_stacks_empty 为 ZMark 成员 start/end 各调，verify_worker_stacks_empty 在 try_terminate_flush 由协调者遍历 worker 本地栈（zMark.cpp:1022-1035）。
- 诊断清扫（0912 令：诊断只留 zVerify* 对应物）：删全部 MRT_TESTABLE_INTERNALS/MRT_GC_UNIT_TESTS 门控 receipt/hook/observer（约 20 族，定义在 Heap/Collector/{Generation.cpp,Remembered.cpp,zRelocate.cpp,TracingCollector.cpp,MarkStripe.*,ExportOwnershipTestObservations.h}、Heap/Barrier/*TestObservations.h、WCollector.h:34-97、各 z/ 文件的 #if 分支）与全部 always-on 阳性对照计数/atexit/LOG（约 14 处：zRelocate.cpp:102-107,414-418,1889-1980,2350-2358；zRelocationSetSelector.cpp:302-342,402-439；zMark.cpp:694-727,758-759,1079,1159-1166；Allocator/zForwardingTable.cpp:63-65,388-398,428-440,505-507；Collector/Collector.cpp:36-40,163-177；TracingCollector.cpp:200-279；RemsetScanStats.h；zDirector.hpp:427-460；PhaseColourContract.h；Allocator/zPage.cpp:52-71）；测试改为对产品接线做故意破坏（gtest friend class *Test 形式保留）。
- 旧目录最终删除：Heap/{Allocator,Barrier,Collector,Verify,WCollector} 整目录、Heap/Heap.h、Heap/HeapTestObservations.h、五个子目录 CMakeLists 合成一份 Heap/CMakeLists.txt 列 z/*.cpp（Heap/CMakeLists.txt:14-18；runtime/src/CMakeLists.txt:235）；windows_x86_64_exports.def 相关导出行；依赖旧路径的门脚本 tests/gc_unit/run_*_arms.py、run_standalone.sh:229,611,765-767 同批改。

**删除清单（extra 行 + 依赖方）**：
- Heap/Verify/CMakeLists.txt + Heap/CMakeLists.txt:18；zMark.cpp:1907-1910 VerifyEmpty 与 17 处调用；zForwarding.cpp:291 ZVerify::Object 调用与 zVerify.hpp:27 public
- 全部 receipt/hook/observer：TracingCollector.cpp:26-119；Generation.cpp:46-271；Remembered.cpp:48-127；Collector/zRelocate.cpp:16-220；MarkStripe.{h,cpp}；ExportOwnershipTestObservations.h；StoreBarrierBufferTestObservations.h；RememberedSet.h（Barrier）；zMark.hpp:145-169,219-248,299-307；zMarkStack.hpp:46-51,165-169；zStoreBarrierBuffer.hpp:42-78；zThreadLocalAllocBuffer.hpp:72-74,122-142,175-176；zRelocate.hpp:107-108；zPage.hpp:129-132,467-473,588-592；zPageAllocator.hpp:18-20,108-110,144-148,171-190,527-537,839-843；zReferenceProcessor.hpp:57；FinalizerProcessor.h:58-64；WCollector.h:161；zRememberedSet.hpp:89-101；zRootsIterator.hpp:29；zHeap.hpp:36-37,140-143；zForwardingTable.hpp:135-136；zForwardingAllocator.hpp:35；zGranuleMap.hpp:73；zPage.cpp:292-299；zRelocate.cpp:1723-1729；zPageAllocator.inline.hpp:448-449；zDriver.hpp:129-131；zDriverPort.hpp:78-95；ObjectModel/MArray.h:65-99；Mutator.cpp:371-380,933
- always-on 计数：zRelocate.cpp:102-107,414-418,987-988,1009-1010,1889-1948,1969-1980,2350-2358；Collector/zRelocate.cpp:16-37；WCollector.cpp:44-49；WCollector.h:743-773；Allocator/zForwardingTable.cpp:63-65,388-398,428-440,443-507；Collector/Collector.cpp:36-40,166；zMark.cpp:694-727,758-759,1079,1159-1166；zRelocationSetSelector.cpp:302-342；TracingCollector.cpp:200-279；RemsetScanStats.h；zDirector.hpp:427-460 + zDriver.cpp:35-45；PhaseColourContract.h；zPage.inline.hpp:1236-1241 + Allocator/zPage.cpp:52-71
- 旧目录整删：runtime/src/Heap/Allocator/、Barrier/、Collector/、Verify/、WCollector/、Heap.h、HeapTestObservations.h；CMake 合并；exports.def 清理

**同批移植的 ZGC 测试**：无（ZGC gtest 无 zVerify 单测；ZGC 对单测只用 friend class *Test）

**不变量判据（≤3）**：
1. 产品翻译单元内不存在 MRT_TESTABLE_INTERNALS/MRT_GC_UNIT_TESTS 门控的观测点，也不存在 always-on 阳性对照计数/atexit 打印；诊断只有 zVerify* 与 log_*
2. ZVerifyRoots/Objects/Marking/Forwarding/Remembered/Oops 六开关下的每条 guarantee 都有产品接线：故意破坏对应机制后精确转红（不是全红）
3. runtime/src/Heap/ 下只有 z/ 一个目录与一份 CMakeLists.txt

**精确转红方式**：① 在 ZRelocateWork::do_forwarding 后不调 forwarding->verify() ⇒ 注入重复 from_index 的构造用例 ZVerifyForwarding 不再拦，精确转红；② ZVerifyRemembered 下 before_relocation 不查 verify_remset_cleared ⇒ in-place 页残留 previous 位用例红；③ 清扫阳性对照：在删除前后各 grep 一次 MRT_TESTABLE_INTERNALS/MRT_GC_UNIT_TESTS/atexit(/RTLOG_ERROR([GCV2] 在 runtime/src/Heap 的命中数，删前 >0 删后 =0，且 gc_unit 测试改写后仍能对每包 red_arm 转红

**看板映射**：
- 新包：无在飞条目；#599/#602 已删部分 MArray 观测位（0x2/0x20/0x80）
- 待核：legacy-residue 组 CMake 拆分行等 JSON UNVERIFIED（verify.md CONFIRMED），见 §待核 U3

## 主控派发说明（0915 23:2x）

- 用户令「回收 workflow 已有的任务，改为 codex 继续执行」：本包回到看板五角色流水线（Status=Implement + 上方 `Blocked-by:` 链，前置全 Done 即由 daemon 派棒）。派出后若主线已含 P01，先逐文件按内容迁入所缺主线改动再建测（禁止 merge/rebase 旧历史）。



## Workflow 成果复用入口（Codex 接管）

- 用户追加要求「之前的 workflow 的成果都回收一下，不要浪费了」：先读 `/root/cj_build/reports/CODEX-TAKEOVER-20260915/recovery/README.md`；原形态普查和包规格仍在 `/root/cj_build/reports/ZGC-SHAPE-ALIGNMENT-0915/PLAN.md`。恢复索引提供原始日志/未写完动作/审查意见；历史结论必须绑定对应候选，不自动作为当前主线结论。
- 已保存远端证据快照：`kkk2:/root/cj_build/workflow-recovery-20260915/`；开始新构建前核对 manifest/收据，避免覆盖唯一证据。

- 主控确认普查归并补充：`/root/cj_build/reports/CODEX-TAKEOVER-20260915/recovery/CENSUS-RECONCILIATION.md`。三组22行差额为旧汇总计数问题；U2的26行、U3的81行已按稳定键恢复原独审结论及既有包消费位置，请本包按表内归属接回清单。保持原infra/待裁属性，不新增包；U1独立核验仍待核，不改变P08原已授权实现范围。此处只恢复历史核验关联，不宣告目标树形态或测试已完成。


## 0916 kkk2资源协调

沿现有共用槽锁执行：runtime两构型构建使用 `/root/cj_build/ops/bin/wf_kkk2.sh build <工作树> <lane>`（内部仍调用同一份kkk2_build_two.sh）；LLVM/std等自定义构建通过该脚本 `bsh <lane> '<远端命令>'`。构建批次共用2个槽，测试共用3个槽；unit/sh子命令和 `cjops windows` 核域预约仍需遵守。不要绕过槽锁另起同一构建；等待构建时先做不依赖结果的源码/报告整理。共享SDK禁止改，产物使用本棒独立目录并保留旧证据。



## 0916 字段与cache前置进入最终清扫闭包

最终全树测试钩子/诊断清扫及冻结验收须消费#577/#607，以及经#607依赖进入的cjcj#48/runtime#646真实String输入与四个CJ_MCC_PackageInit桥；避免P16先完成后再新增初始化测试钩子/接口而使清扫闭包失效。这是最终闭包依赖，不串行前面的形态包。四签名当前均native AS0/i32、无AS1/sret，既有ManagedABIGateVerifier会calls_passed，report-only表无需为本轮增加名字或重建LLVM；本包只复核冻结实际IR签名/桥与分类证据，不机械新增白名单。若未来签名改变含managed引用，重新审查精确契约，不能前缀放行。


## 0916 公共卸载协议发布前置

新增公共卸载协议#648等待#646已审交付后独立修复/验证，保持#48/#607现有方向。最终全树清扫消费本包稳定image资格及testable同步接缝，不能在其之前完成冻结；不扩大#646为整个loader重写。


## 0916 Windows导出表实际重复与最终生成归属

# Windows导出表具体债项：归现有P16与发布闭包

#646发现当前表g_cjLoadGoodMask与CJ_MCC_PostWriteRefField都使用@3487，读证属实：runtime/src/windows_x86_64_exports.def:2981/3124。当前Windows正式流程不是直接用这份表强制链接：runtime/CMakeLists.txt:549–555从完整MinGW link生成raw.def；runtime/src/CMakeLists.txt:259–267随后运行generate_windows_exports.py check，parse_exports:31–53会拒绝重复ordinal。

该表的清理已经属于P16/#627（PLAN.md:603/609），本次把明确的重复项和四对PackageInit新增桥输入附入该现有owner，不新起相同导出表修复包。#646的Linux/OHOS-host证据不外推Windows导出资格；临时源表编号也不被当作最终Windows验收。

#627在最终形态/ABI收口后必须沿实际Windows cross runtime link的raw.def，通过既有generate_windows_exports.py write生成规范表并check；不得只手动选空号或删一个重复项来宣称全部导出闭包一致。保留新增MCC/CJ_MCC四桥、DATA属性与实际被引用符号，旧形态导出按最终source删除。用现有检查器真实拒绝重复/错误表的对照，验证最终规范表与完整链接一致，不另造装置。

#47最终5平台输入/发布pin消费该规范化之后的runtime候选与实际Windows runtime产物；这是原Windows真实producer/consumer闭包的一部分，不用Linux运行结果替代，也不要求当前#646额外重建整套Windows/LLVM/stdlib。该发布债在#627和#47真实证据齐备前保持未闭合。

<!-- 主控落正文 0919 13:0x，来源 reports/REPORT-sym_cangjie_runtime_711_synthesize_r5739435286.md -->
## gc_unit D 档删除清单（主线 f79c1e94 失败集归并，#711）
以下测试断言的是 ZGC 无对应物的自研机制（转发收据/值根 currentize/dual-carrier heal/
lookup 状态机/RegionList 挂链），0912 令：直接删测试。产品面核查（主线 f79c1e94）：
`git grep -c LookupTo -- runtime/src` = 0（rc=1）；`git grep -c SeedValueRoots -- runtime/src` = 0（rc=1）。
阳性对照：RegionList 在 runtime/src 仍有 23 文件命中（旧形态残留，测试同样删，产品清扫随 P03/P05）。

删整套件（文件级）：
- 套件 ForwardingPublicationProduct（default 21 / testable 14，含 testable 独有 CompletedPageResolvesThroughForwardingTable）
- 套件 ValueRootCurrentization（13）
- 套件 LoadHealDeliveryProduct（default 6 / testable 7，testable 含 MajorDispatchRemapsLiveRemoteArrayField）
- 套件 ForwardingLookupWitness（2）· FindToPublicState.QueryableMissIsObservable
- ForwardingNoGeometry.ForwardImplTryLockCopiesWithoutPrebuiltMapping
以上主体在 runtime/tests/gc_unit/clear_entries_product_unit.cpp（:717-:2835 一带）。

随 RegionList 删除：
- ZListPort.InsertAndRemoveFirst / RemoveFirstAndLast（test_region_list.cpp:88,:105，注释自述 "No ZGC counterpart"）
- RegionRetirement.CompactInPlaceLeavesRegionOnAListACollectorWalks（test_pinroot.cpp:138）

## gc_unit 主线红增补（#711 归并）
ZVerify.ForwardingTableChecksLiveAccounting（test_verify_fail_close.cpp:104；ZForwarding::verify 活对象账）
与 ZVerify.BeforeRelocationRejectsMissingRememberedField（:131；zVerify.cpp:531-609 源字段必须在 remset）
主线红：verify 未 fail-closed / 缺 remembered 未拒。

## gc_unit T 档（改断言，#711 归并）
- `RegionAge.YoungAgeRoundTrip`（test_region_age.cpp:20-32）：产品 `GetYoungAge`（zPage.inline.hpp:789-791）与 ZGC（zPageAge.hpp:30-46）均为 eden=0。断言改为：`GetYoungAge(eden)==0`、`GetYoungAge(survivor14)==14`、`GetYoungAge(old)==0`。
- `RegionAge.MaxYoungAgeBound`（:35-40）：断言 `GetYoungAge(survivor14)==14`，MAX_YOUNG_AGE(63) 仅作上界（zPage.hpp:723-725），⛔ 不断言相等。

---

# 共同前言（⭐ 每个 stage 的 prompt 都以它开头）

你在一次**无人值守**的编排会话里工作。⛔ 不要要求人来做后续动作。

- 冻结坐标写在本条 issue 正文里；⭐ **你必须自己回读一遍并记 rc**，⛔ 不许从别处推。
- ⛔ **不许 push 到任何官方远端**（`gitcode.com` / `CangjieFork` / `upstream`）。可推的只有 `cjcj-dev/*`。
- ⛔ **不许改共享安装** `/root/sdks/**`、`/root/.cjv/**`。
- ⭐ 一切产物落 `/root/cj_build/` 之下。kkk2 上仍用 `/root/<棒名>/`。
- ⭐ commit 作者固定 `Zxilly <zxilly@outlook.com>`；⛔ **禁止任何 AI 署名 / Co-Authored-By**。
- ⭐ 编译与测量一律去 kkk2：`bash /root/cj_build/tools/box.sh kkk2 '<命令>'`，命令前加 `ulimit -c 0`。
- ⭐⭐ **发现了新问题就开 issue**：`cjops sym issue new --repo <仓> --title <题> --status Triage --blocker <① correctness|② concurrency|③ perf|debt|infra>`；
  ⭐ 或在报告末尾写 `SYM-NEXT: … new-issue: <仓> | <题> | <档> | <归属>`（⭐ 0911 起只有 Explore/Synthesize 的交付会被编排器真开；Implement/Review/Merge 写的 new-issue 行只落成 `<!-- sym-new-issue-skipped -->` 评论交主控手开；同仓同题与同份内重复一律跳过）。
  ⛔ 别把发现塞进报告正文就算交代 —— ⭐ 那正是旧流程漏掉事情的地方。
- ⭐ 只在真正的外部阻塞（缺工具/权限/凭据）时提前停；停就交终态报告并**写明缺什么**（⭐ 编排器会把它转 Blocked）。

## ⭐⭐⭐ 一条 issue ＝ 一个问题；你有权判它【不成立】并关掉

**每条 issue 只装一个问题。** 如果你在做的过程中发现它其实是两三个问题 ⇒
⭐ **拆**：把额外的开成新 issue（`cjops sym issue new …`），本条只留一个。

⭐⭐ **你有权把本条 issue 判为【不成立】并关闭** —— 这不是逃避，是流程的一部分。
适用场景：问题描述基于错误前提 · 我方其实已有等价实现 · 已被别的改动取代 · 与另一条重复。

⛔⛔ **但关闭必须带证据，⛔ 不接受"看起来没问题"**：
```
SYM-CLOSE: <invalid|duplicate|superseded>
理由：<一句话>
证据：<file:line 两侧锚 / 命令 + rc / 重复的是哪条 issue>
```
⛔ `SYM-CLOSE:` 后面**只能是这三个词之一**；⭐ 不关 issue 就**整行不写**。写成说明文字（例「不在本轮关 issue」）会让报告头解析失败，整份交付按 invalid 收割（0910 cangjie-runtime#62 实付一轮）。
⚠⚠ ⭐⭐⭐ **两个方向都会错，⛔ 不要以为"关掉"比"修"安全**：
本项目实账是主控填的「疑似缺」被实核 **12 次全错**，方向一致地**低估我方已有的** ——
⭐ 那说明「判它不成立」这个方向**也**同样容易错。⇒ ⭐⭐ 判**不成立**之前必答与判 bug 同样的三问：
① 我按【功能】找的还是按【名字】找的？② 去**调用层**查过实际用哪个变体没有？③ **生产端与消费端都比了吗**？

---

# Implement 档的契约

⭐⭐⭐⭐⭐ **阶段二·行为修复（0914 起；形态对齐已收口）**：逐失败用例三分类——(i) 所测机制已被对齐包删除 ⇒ 整条删除并登记归属包；(ii) 用例期望与 ZGC 语义不符 ⇒ 按 ZGC 改期望并给 zX.cpp 锚；(iii) 产品行为与 ZGC 不符 ⇒ 按 ZGC 修产品并给锚与最小改动。⛔ 不许为过绿弱化仍有效断言、加 known_failures/豁免/开关/`#if 0`/常量 false。
- 每处产品修法给故意破坏后**精确转红**的证据（红在目标断言、证明目标断言被执行到）；「先看 ZGC 怎么做」——每条修法先给 ZGC 锚再动手。
- kkk2 用 `/root/cj_build/ops/bin/kkk2_build_two.sh <工作树> <棒名>` 两构型构建（-j 跑满 192 核），再 `run_standalone` 三臂各一次，如实报告本包名单用例的通过/失败集与全套件计数；别包前缀的失败不算本包打回理由，⛔ 不重跑取绿。
- 范围以 issue 正文名单用例对应的机制为边界；共享夹具/相邻缺陷写进后续项，不擅自扩。同分支返工；交付前 merge cjcjdev/main。
- DONE 条件：本包名单用例全部处置（删/改期望/修产品各有登记）、两构型 rc、三臂结果如实记录、产品改动有转红证据。
- ⭐⭐ 报告必须**逐字**含三行：`UNIT_DEFAULT_RC=<default 臂 rc>`、`UNIT_FILLER_RC=<filler 臂 rc>`（⭐ 0915 定义：filler 臂＝**同一 default 构型 ELF 以 `CJRT_HEAP_FILLER=0` 环境再跑一遍单元集**，见 tools/gate_unit.py:320；⛔ 不是 testable/MRT_TESTABLE_INTERNALS 臂，⛔ 不能拿 testable 的 rc 填）、`UNIT_OHOS_RC=<rc>`（OHOS 构型构建/运行**尝试**的真实退出码，如 build 失败 rc=1、runner 先验 rc=21、lineage rc=74；只有 kkk2 上根本无法尝试时才写 `NOT_RUN(原因)`——审查会把可尝试而未尝试的 NOT_RUN 打回），各附 kkk2 日志路径（凡改了 tests/gc_unit、构建系统或做了 rebase/merge 主线都要）；缺任一行审查按 B 类打回（0914 已实付 4 轮）。纯测试/期望修正的交付同样要。

---


## ⛔⛔⛔ 交付前**必须自己过**的红臂三条（⭐ 不过就别交，⛔ 审查一定打回）

⚠ ⭐⭐ 实况：**最近三份实现全栽在第 ① 条上** —— ⭐ 判词都是同一句：
「**断了承重点，套件仍全绿**」。⭐ 所以这三条**不是形式**，⭐ 是你这一轮过不过的关键。

```
① ⭐⭐ 断了承重点，必须【真的有测试变红】
   ⛔ 全绿 ⇒ **测试没穿过那个承重点**，⛔ 不是「说明没问题」
   ⭐ 断的必须是**产品侧**的真实出口，⛔ 不是测试内的 helper 或模型副本
   ⭐ 生产端与消费端**各断一刀**（⭐ 机制是成对的，⭐ 只断一端证明不了接线）

② ⭐⭐⭐ 红必须红在【目标断言】上
   ⭐ 查有没有**更早的断言**先失败/先抛异常，把目标那一行**遮住**
   ⇒ ⭐ 把「前置存在性断言」与「目标不变量断言」**分开**，⭐ 或让前者不致命
   ⚠ 实账：一条棒的切刀确实只让目标测试红，⛔ 但唯一 FAIL 是**更早那行**
      `EXPECT retained` —— ⭐ 它立即抛异常 ⇒ **目标顺序断言那一行根本没执行**

③ ⭐⭐ 证明目标断言【确实被执行到】
   ⭐ 单独 filter 跑它，⭐ 或让它通过时也留一行可见输出
   ⛔ 「它在文件里」⛔ 不等于「它跑了」
```

⭐⭐ 报告里请**逐条**给证据：切的是哪一行（`file:line`）· 切前/切后/恢复三臂的 rc 与计数 ·
目标断言被执行到的证据。⛔ 只写「已做精确转红」不算 —— ⭐ 审查会去核那三个数。

⚠ ⭐⭐⭐ 还有一条**只在这里说**：⭐ 若你**做不出**精确转红 ——
⭐ 那多半**不是你测试写得不好**，⭐⭐ 而是**产品接线本身没接上** ⇒
⛔ 别去改测试让它变红，⭐ 回去看那条产品路径**到底有没有被调用**。

## ⛔⛔⛔⛔ 红臂**有效性**的闭环前提（⭐ 0909 方向裁决原文，⛔ 逐字生效）

> **红臂有效性以「产品身份一致、真实入口经过、产品结果进入断言」的闭环证据为前提；
> 提交者必须逐承重面给出该闭环。闭环未成立的转红，不计作该产品行为的验收证据。**

⚠ ⭐⭐⭐ 这条**优先于**上面的红臂三条 —— ⭐ 三条全做到、⭐ 而闭环不成立 ⇒ ⛔ **仍然不算通过**。
⭐ 实账：`#3` / `#25` / `#9` 三份实现被同一个理由打回，⭐⭐ 共同病灶**不是**「产品接线不存在」，
⭐ 而是「**现有测试不能支撑所需的产品行为判词**」⇒ ⛔ 再切一刀也不解决。

⭐⭐⭐ **「真的走过产品接线」的三段判据（⭐ 三段全中才算闭环）**：

```
① ⭐⭐ **真实进入** —— 被测对象就是**产品构型下的产品实现**本身
   ⛔ 不是测试内另编的同名副本 · ⛔ 不是 helper · ⛔ 不是手工拼起来的三段调用
   ⇒ 要给：产品身份锚（`file:line` + 该符号在**被测产物**里的存在证据）

② ⭐⭐⭐ **实际经过并【被观察】** —— 那条产品路径在本次运行中**确实执行过**，且**留下可读的痕迹**
   ⛔⛔ **静态可达性不够** · ⛔⛔ **命中计数不够** · ⛔⛔ 「这个函数执行过」**也不够**
   ⇒ ⭐ 要的是：**产品产生的那个结果值/状态**被读出来，并**进入断言**

③ ⭐⭐⭐ **因果敏感** —— 改动产品那一处，判词**跟着变**；不改则不变
   ⛔ 编译失败造成的红 **不算** · ⛔ 加载失败造成的红 **不算**
   ⛔ 入口无条件报错造成的红 **不算** · ⛔ 直接改断言造成的红 **不算**
```

⭐⭐ **逐承重面**给出上面三段，并**自评一档**（⭐ 审查会按同一把尺复核）：

```
✅ 闭环成立   ⇒ 三段齐备、各有产物（命令 + rc + 输出位置）
⚠ 闭环存疑   ⇒ 三段中有一段只有【推理】没有【产物】 ⇒ ⛔ 不计作验收证据
⛔ 闭环不成立 ⇒ 缺段，或红来自上面列的四种「不算」之一
```

⚠ ⭐⭐ **自评 ⚠ 或 ⛔ 不是失败** —— ⭐ 如实写出来 ⇒ 这一轮按「证据不足」收口；
⛔ **谎报 ✅ 才是失败**（⭐ 审查一定会去复核那几个数）。


## ⛔⛔ 路径纪律（⭐ 本项目实付过：一条棒在错的仓里 `cd ops` **成功了**，随后把产品码提交进了 main）

⭐⭐⭐ 协调面（`coord/` · `design/` · `advisor/` · `tasks/` · `seed/` · `bin/`）**一律**是
**`/root/cj_build/ops/…`** —— ⛔ 各仓工作树里那份 `ops/` 是**该仓自己的**文档，⛔ 与本轮无关。
⇒ ⭐ 见到任何相对的 `ops/…`，⭐ 一律按 `/root/cj_build/ops/…` 读；⛔ 别去猜、⛔ 也不必为此 advise ask。
⭐ 你写报告引用协调面文件时**同样写绝对路径** —— ⭐ 读你报告的人不一定站在同一个 cwd 里。

## ⭐⭐⭐⭐ 收尾第一步：调 MCP 工具 `sym_deliver` 登记结构化交付

⭐ 你的会话里挂了一个叫 **`cjcj-sym`** 的 MCP 服务器，⭐ 它有两个工具：
- `sym_task(lane)` —— 读回本份任务书原文（含冻结坐标）
- **`sym_deliver(...)`** —— ⭐⭐⭐ **登记交付。编排器按它转档。**

⭐⭐ 至少要传：`lane`（＝你的棒名，逐字符）· `progress`（`WIP` 续跑／`DONE` 停）· `summary`；
⭐ 终态还要给 `next_stage`（转哪一档）**或** `close`（判它不成立，⭐ 理由与证据缺一不可）。
⭐ 逐条结论走 `claims`：`method` ＝ `read`／`test`／`measure`／`control-arm`，
  ⭐ 每条都要 `anchors`（file:line）；⛔ `read` 之外的都隐含「跑过」⇒ **必须给 `n`**。
⭐ 本轮发现的**别的**问题走 `new_issues`（⭐ 每个问题一条，⛔ 别塞进正文当交代）。

⛔⛔ 校验不过会**逐项**告诉你缺什么 ⇒ ⭐ 改完**再调一次**，⛔ 别改成散文绕过。
⭐ **边做边调**：做到一半就用 `progress="WIP"` 调一次，⛔ 别攒到最后（⭐ 会话可能被换掉）。

⚠ ⭐⭐ 如果你的环境里**没有**这个工具（⭐ 换了通道、或服务器没起来）——
  ⇒ ⭐ 照下面的**散文兜底**写，⛔ 别因此停下；⭐ 但在报告里写一行 `MCP: 不可用`。

## ⭐⭐ 兜底：报告末尾的**机器可读**行（⭐ 没有 MCP 时用这条）

```
SYM-PR: <repo>#<PR 号>
```
⛔ 没有 PR ⇒ 本档**不算交付**（编排器会把它退回 Implement 重派）。

⭐ 发现了**别的**问题 ⇒ 同样用机器可读行开新 issue（⛔ 别塞进正文当交代）：
```
SYM-NEXT: stage=<下一档> new-issue: <仓> | <一句话题目> | <档> | <① correctness|② concurrency|③ perf|debt|infra>
```

⭐⭐ 判本条 issue **不成立**并关掉（⭐ 这是流程的一部分，⛔ 但必须带证据）：
```
SYM-CLOSE: invalid        # 或 duplicate / superseded
理由：<一句话>
证据：<file:line 两侧锚 / 命令 + rc / 重复的是哪条 issue>
```
⛔ 缺「理由」或「证据」⇒ ⭐ 编排器**不予关闭**（⛔ 不接受「看起来没问题」）。
<!-- DELIVERY-PROTOCOL-V1 -->
---

本轮角色：`ROLE=implement`。

# 统一交付协议 v2（规范版，0912）

> 完整叙事版存 `ops/coord/DELIVERY_PROTOCOL-full-v2-0911.md`（含每条规则的实付来源）；本文件只保留规则，节号不变。
> 一句话：每一条主张都必须携带「它是怎么被验证的」。五种角色（explore/synthesize/implement/review/merge）全部适用。
> 四关递进：§1 你说的是什么 → §2 你测的是产品吗 → §3 你测到了全部吗 → §4 这份证据可信吗。审查按此顺序查，打回时写清卡在第几关。

## 0 · 工作目录纪律（0906 用户令）
- 本机一切产物落在 `/root/cj_build/` 之下：工作树 `/root/cj_build/<repo>_wt/<name>`，构建/临时 `/root/cj_build/agent_scratch/<棒名>/`，证据 `/root/cj_build/reports/EVIDENCE-<棒名>/`，报告 `/root/cj_build/reports/REPORT-<棒名>.md`。⛔ 不在本机 `/root/` 下新建顶层目录。
- kkk2 上仍用 `/root/<棒名>/`；⛔ 不改远端布局。
- `EVIDENCE=` 必须带主机前缀：`local:/root/cj_build/…` 或 `kkk2:/root/…`；仓内文件可写绝对路径。审查方：本机查不到先去 kkk2 查（`bash /root/cj_build/tools/box.sh kkk2 'ls -ld <路径>'`），⛔ 不直接判「不存在」。
- 任何 git/写入命令以 `cd <绝对路径> &&` 开头；相对路径在 `cd` 后会读错文件。

# §1 · 报文形式
## 1.1 报告头（前五行位置固定）
```
第1行  PROGRESS=<WIP|TRIAGED|DONE> · verdict=<一句话判词> ｜尺=<装置> <关键量>=<值> N=<样本数> · LANE=<棒名>
第2行  DELIVERY_REF=<仓>|<分支>|<40位sha>      或 DELIVERY_REF=none|no-code|<一句话原因>
第3行  SIDE_EFFECT: <一句话>                    无副作用写「无」
第4行  ROLE=<explore|synthesize|implement|review|merge>
第5行  EVIDENCE=<带主机前缀的绝对路径,逗号分隔>   或 EVIDENCE=none
```
- verdict 必须点名量它的尺；多把尺就按尺分开写，⛔ 不让弱尺借强尺的绿。`*_OK`/`*_PASS` 这类 token 先读打印它的那一行，问它什么条件下不打印——答不上来它就不是判据。
- 三态：`WIP`=还在做（派发器续跑）；`TRIAGED`=停下等裁决；`DONE`=做完。还在做就写 WIP；**提问之后仍保持 WIP**。
- `DELIVERY_REF` 只认两种形态；sha 必须真实存在的 40 位。
- 提交作者固定 `Zxilly <zxilly@outlook.com>`；提交与报告中不加任何 AI 署名。
## 1.2 CLAIM 块（每条实质主张）
```
CLAIM: <断言一句话>
  METHOD: <read|measure|test|control-arm>
  EVIDENCE: <file:line | 带前缀绝对路径 | 可复跑命令>
  N: <样本数>          measure / control-arm 必填
```
`read` 给 file:line；`measure` 给结果文件 + N；`test` 给测试名 + 故意破坏转红的证据；`control-arm` 给两臂路径 + N。
硬规矩：① measure/control-arm 必须给 N；② 任何「没有/为 0/全绿」的主张必须配阳性对照 CLAIM（恒 0 与恒大都要对照；读数之前先确认那个程序真的运行了——看 rc，别只看 stdout）；③ 涉及负载的数必须带 ELF sha256 + SO 血缘 stamp + 核域 + 两端 load average。
## 1.3 `## FALSIFIED`（必须存在）
任务书里被你证伪的前提逐条列出并带证据，没有写「无」。推翻前提是加分；自我更正也写这里，⛔ 不作为打回理由。

# §2 · 第一关：你测的是产品吗
## 2.1 `## 产品接线证明`（implement + DONE 必须有）
表：`测试名 | 它调的产品函数(file:line) | 断开这条接线测试会红吗 | 凭什么`。三条自查（任一为是 ⇒ 没在测产品）：测试有没有自己重编/拼一份被测组件；有没有把中间值手工喂给下游；把产品里那个调用整行删掉测试还绿吗。
## 2.2 断线臂（必须交实际产物，光有表不算）
① 改产品源码断掉测试依赖的那条调用 ⇒ 存 cut.diff；② 重编产品 SO ⇒ 存构建 log；③ 复跑同组测试 ⇒ 存 cut-*.log 与 rc，必须 rc≠0；④ 恢复源码复跑 ⇒ 存 restored-*.log（回绿）。节里必须出现证据文件的绝对路径。
## 2.3 断在承重点
给产品树的 file:line（不在 tests/ 下），回答「产品运行时从哪个调用点走到这条路，我断的是不是那一个」。只有测试会走到的点不是承重点。
### 2.3.1 断开的位置必须是基线里已存在的产品调用点（机器判定）
cut.diff 里被删/改的每一行必须在 `git show <基线sha>:<文件>` 里逐字存在且不在候选新增行里；至少一处断开落在真实入口函数（runtime 仓清单 `/root/cj_build/ops/coord/PHASE_ENTRIES.txt`，tools 仓 `PHASE_ENTRIES-tools.txt`）。交付前必跑并附 JSON：
`tools/entry_cut_check.py --repo <工作树> --base <基线sha> --head <候选sha> --cut cut.diff --phase-entries /root/cj_build/ops/coord/PHASE_ENTRIES.txt --json <证据目录>/entry_cut_check.json`；rc≠0 不送审。
### 2.3.2 先交 producer→consumer 顺序表，再改产品码
数据从 producer(file:line) 到每个 consumer(file:line) 的顺序，标出修改必须落在哪一点之前；断线补丁至少一处断在表里的 consumer 上。
## 2.4 跨文件的臂是瞬态的
允许对邻棒文件做瞬态受控破坏，条件：只在隔离树；破坏前后各记 sha256 且相同、`git status --porcelain -- <文件>` 为空；报告写明动过谁的文件、属谁的域、最终零 diff。
## 2.5 四种合法例外（都必须写明缺口，推迟≠豁免）
- 例外一 · 当前无产品消费者的新 utility：如实写「基线产品消费者=0（附检索命令与输出）」「首个消费者是 X，接线证明推迟到它迁入时补」；⛔ 不断假接线点、⛔ 不加测试专用导出凑臂。
- 例外二 · header-only 模板：(a) 在头文件本体故障注入 ⇒ 复跑 rc≠0 且只红该项 ⇒ 恢复回绿；(b) 证明不存在第二份拷贝（检索命令与输出）。
- 例外三 · 唯一承重点：给唯一性的源码证明（逐面 file:line）、至少一份单面臂（最可能绕过的那一面）、反向对照（那一刀红的项全部属于该机制）。
- 例外四 · 交付对象本身是 harness/analyzer/门脚本：承重点=它把变量/产物/记录送入实际执行的那一行；切它并真实运行同一集成入口；⛔ 不为形式要求改无关产品函数。报告给 `git diff --name-only` 证明无 runtime 产品码改动、入口与承重点 file:line、切断转红/恢复回绿产物，并明写该证据只证明装置本身。

# §3 · 第二关：你测到了全部吗
## 3.1 `## 承重面清单`（implement + DONE 必须有）
由检索命令导出（命令与输出都贴）：表 `消费者/分支 | 产品 file:line | 有无臂 | 臂的产物路径 | rc`。硬规矩：① 清单必须机械导出；② 分支轴都要展开（full/range · 成功/失败 · pre/post · 静态/动态 · 单/多 worker · 公共/内部入口）；③ 判「不需要臂」要写理由，「我觉得等价」不算，「它没有产品调用者（附检索输出）」可以；④ grep 命中≠语义命中——贴每个命中所在整行，逐条确认是调用点还是字符串/注释/同名；反向也成立（版本化符号 `SYMBOL@@VERSION` 会让精确匹配漏判），凡拿 nm/grep 判「符号在不在」必须做双向对照。
## 3.2 枚举向上走到运行时入口
① git grep 出直接消费者；② 从每个直接消费者向上走调用链到运行时真正发起的入口（mutator 操作 · GC 相位 · 线程入口 · 屏障发射点 · safepoint 动作）；③ 臂放在入口；④ 每一层贴检索命令与输出。臂全在同一个文件 ⇒ 多半停在直接调用者。
## 3.3 `## 测试增删`（implement + DONE 必须有）
相对基线 sha 的测试名**集合差**（新增/删除/改名），每条删除或替换写理由（可接受：「已被 X 完全覆盖且 X 更严」）。⛔ 别为了让阻断消失而删测试。

# §4 · 第三关：证据本身可信吗
## 4.1 构型：宏门控的产品形态
默认（产品编译）保持内联/无导出/无 friend；定义测试宏时才导出/去内联/开 friend；必须是编译期开关。判据：关掉宏产品还正确吗——仍正确可门控；不正确则是正确性装置，⛔ 不许门控。每条臂标注构型并回答能否外推到产品构型（存在性可外推，性能一律不可）。
### `## 双构型证明`（宏门控交付必有）
表 `构型 | cmake 变量原样 | 构建 rc | 该项状态 | 产品 SO sha256`：宏关构建必须 rc=0；宏开该项必须 PASS 且检查在某处真的跑过并能失败。审查加一问「你建过宏关的那一种吗」，只建一种 ⇒ B 类。
## 4.2 三臂必须是同一构型
每臂记测试 ELF sha256 + 产品 SO sha256 + 构建时刻（晚于最后一次源码改动）。判据：① 三臂里只有承载那一刀的产物变；② 承载刀的产物：绿=恢复，断线≠它们；③ 其余产物三臂逐字节相同；④ 三臂总项数相等，不等要逐项交代。刀在产品 SO 且测试动态链接时，测试 ELF 三臂必然相同。
## 4.3 产物哈希在产生的那一刻捕获
① 链接完成后立刻 sha256sum；② 或让测试跑在被保留的副本上（LD_LIBRARY_PATH 指向它）；③ 或运行期读 `/proc/<pid>/maps` 就地哈希。⛔ 不许事后重建。三臂用同一种办法。
## 4.4 两臂比较先确认配方相同
写明两臂各走哪条构建入口且必须相同；比较前先让同一臂比自己得噪声底，噪声底高于待测差异则判据作废。

# §5 · 角色专属要求
| 角色 | 要求 |
|---|---|
| explore | DELIVERY_REF 为 `none\|no-code\|…`；交付统一对齐表 `ZGC 做法(file:line) \| 不变量 \| 我方对应(file:line 或"无") \| 差在哪`，每行落到 ✅等价 / ⚠形态不同（须继续判到 (a) bug 进修复清单 或 (b) 等价降 ✅）/ ⛔真缺（写违反哪条不变量）；判真缺门槛高：先答「按功能找的还是按 ZGC 名字找的」「调用层查过实际用哪个变体了吗」「生产端与消费端都比了吗」 |
| synthesize | 交付可直接派的实现任务书 + 矛盾消解记录 |
| implement | **形态对齐期（WORKFLOW alignment_mode: true）例外：DONE 只要 ZGC 对应表＋删除清单＋grep 零命中＋构建 rc 记录，⛔ 不要求下列任何一项；⛔ 不许为绿改测试/加豁免/假删除。** 常态：DONE 时至少一条 `METHOD: test` 的 CLAIM（精确转红，一破全红=过度捕获）；必有 `## 产品接线证明` `## 承重面清单` `## 测试增删`；红必须红在目标断言上（查有没有更早的断言先失败），并证明目标断言确实被执行到 |
| review | 必写「放行」或「打回」；放行给两栏「新的放进来什么 / 旧的挡住了什么」；收割前四件核对：授权记录 · 会话 ID · 报告产出事件 · 候选实现者身份（不能只读报告头自称的角色） |
| merge | **形态对齐期例外：不跑 gate_all、不写 SYM-GATE，只做内容核＋冲突扫描。** 常态：逐文件内容核（`git grep -c <特征符号>`），`git merge-tree` 干净对静默回退是盲的；SYM-GATE 的 base.sha=派发冻结基线、tip.sha=候选 head，主线前进后的预览臂写散文；门红/任何原因不推 ⇒ 交付前把本地 main 退回远端并写明前后 HEAD |
## 5.1 出规格给别人用时标坐标基线
规格必须写「坐标基于 `<40 位 sha>`」；转派方先核锚在目标树存在再派。
## 5.2 审查打回标准 A / B / C
- A 真缺陷：实现是错的（行为错 / 既有守卫或测试被削弱 / 违反裁决）⇒ 打回。本提交自己承诺的范围只兑现一部分 = A。
- B 枚举/记账/证据链不成立（方法够不到虚派发/函数指针/模板/宏/非限定调用；方法对但没跑遍；外推）⇒ 打回。定责时别漏「合并动作本身引入」：两侧各自绿≠合并后绿，二分端点含 merge commit 的两个父。
- C 方法成立但靠额外洞察找到第 N+1 个入口 ⇒ 不是打回理由，记 `## 后续项`（每条写成可直接派的一句话）并放行。别人留下的旧接口路过发现 = C。
审查的价值在证伪，不在打回率；只找到「还可以更严」的改进 ⇒ C 类。

# §6 · 交付前自检
`/root/cj_build/tools/cjops deliver check --lane <棒名>`：rc=0 只代表报文形式齐了（头五行、40 位 sha、CLAIM 有 METHOD、measure 有 N、必需节都在），⛔ 不代表证据真、臂断对、枚举全、三臂同构型——那些只能靠对抗审查。⛔ 别把 rc=0 写成「已合协议」向下游转述，正确说法是「形式检查通过；实质由 review 判」。
留下的门（判据都是读真实状态）：KKK2-DISK · ANCHOR-MISMATCH · STALE-WORKTREE · CLASSIFIER-BLOCKED · CANCELLED · REPORT-PATH。已删的启发式门（PRIOR-ART/DIAG-UNUSED/NEGATIVE-PRIOR/CORE-DOMAIN/adviseChannel）⛔ 别加回来。

# 附录 · 最小合协议例子
```
PROGRESS=DONE · verdict=终止屏障本体在场，主控「只有观测」判错 ｜尺=源码读证 N=不适用 · LANE=demo
DELIVERY_REF=none|no-code|对齐轮，只读
SIDE_EFFECT: 无
ROLE=explore
EVIDENCE=local:/root/cj_build/reports/REPORT-demo.md

## 对齐表
| ZGC | 不变量 | 我方 | 差在哪 |
|---|---|---|---|
| zMarkTerminate.inline.hpp:… | 所有 worker 无私有工作且共享栈空才可退出 | Mark.cpp:1404-1419 | ✅ 等价 |

CLAIM: 我方终止屏障本体在场
  METHOD: read
  EVIDENCE: runtime/src/Heap/Collector/Mark.cpp:1404-1419

## FALSIFIED
任务书称「只有观测计数」⇒ 源码证伪：观测在 TracingCollector.cpp:23-54，本体在 Mark.cpp:1404-1419。
```

