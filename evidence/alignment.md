# D06b 形态对齐记录

协调文档引用均来自 /root/cj_build/ops/；本文件是候选交付证据，待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

坐标：冻结 `5e04db890b22df66bc8875d3557c55ff7ef9177d`；合入主线 `8e1455a55012b2068c7a65b8e3f9cfb3437d086f`；构建代码 `edf6c87d07a0cceb3dcdc934cb1af6a21e15bf9a`。
R = 本候选的 runtime/src/；Z = /root/cj_build/reference/jdk/src/hotspot/share/gc/z/。

## ZGC 函数对应表

这些是本包必须保留的实际检查及其调用。本轮删除重复诊断包装；对应断言已在冻结源码的本体内，未新增第二条检查路径。表不声称 D06b 之外整个 collector 已完成形态对齐。

| ZGC 函数/开关 | 我方本体或直接消费者 | 本轮处置 |
|---|---|---|
| Z:zVerify.cpp:489 before_zoperation | R:Heap/z/zVerify.cpp:48 BeforeZOperation | 保留 roots 接线 |
| Z:zVerify.cpp:496 after_mark | R:Heap/z/zVerify.cpp:52 AfterMark | 保留 strong roots / objects |
| Z:zVerify.cpp:507 after_weak_processing | R:Heap/z/zVerify.cpp:57 AfterWeakProcessing | 保留 weak-inclusive 校验 |
| Z:zVerify.cpp:363 roots_strong / :386 roots_weak | R:Heap/z/zVerify.cpp:92 RootsStrong / :103 RootsWeak | 保留实际根访问 |
| Z:zVerify.cpp:119 z_verify_oop_object / :467 objects | R:Heap/z/zVerify.cpp:122 Object / :191 Objects | 保留对象图检查 |
| Z:zVerify.cpp:131 z_verify_old_oop / :179 z_verify_possibly_weak_oop | R:Heap/z/zVerify.cpp:140 Oop | 保留强弱引用判据 |
| Z:zVerify.cpp:587 on_color_flip | R:Heap/z/zVerify.cpp:236 OnColorFlip | 保留 store buffer 信息 |
| Z:zVerify.cpp:610 before_relocation | R:Heap/z/zVerify.cpp:244 BeforeRelocation | 保留原页 remembered 校验 |
| Z:zVerify.cpp:741 after_relocation / :763 after_scan | R:Heap/z/zVerify.cpp:296 AfterRelocation / :302 AfterScan | 保留目标页 remembered 校验 |
| Z:zMark.cpp:1022 verify_all_stacks_empty | R:Heap/z/zMark.cpp:2023 VerifyAllEmpty；调用 :1348 | 保留 thread/stripe 空栈断言 |
| Z:zForwarding.cpp:369 verify | R:Heap/z/zForwarding.cpp:276 verify | 保留 owner、地址、唯一性、live accounting |
| Z:zBarrier.inline.hpp:40 assert_transition_monotonicity | R:Heap/z/zBarrier.cpp:36 AssertBarrierTransitionMonotonicity；R:ObjectModel/RefField.h:328 调用 | 删除诊断包装；现有六个本体 CHECK 保留 |
| Z:zBarrier.inline.hpp:72 self_heal | R:ObjectModel/RefField.h:304 ZgcSelfHeal / :322 前置 CHECK | 保留重试、fast-path 退出与单调性检查；删除 census / spin alarm |
| Z:z_globals.hpp:78 / :81 / :84 / :87 / :105 / :119 | R:Heap/z/zVerify.cpp:35 六个 ZVerify* 配置 | 这些实际校验开关保留；旧诊断开关删 |

## 消费端清理

只删除本包诊断生产/消费及其专用参数，保留周围的原产品操作。下表给复核入口；全部修改文件由 git diff 枚举。

| 我方函数/锚 | 删除的诊断 | 本体对应或排除依据 |
|---|---|---|
| CompilerCalls.cpp:234 及各 MCC_New* 分配入口 | correlation tag 消费/拒绝记录 | 无 ZGC 对应账本；分配本体未改 |
| Heap/Allocator/zForwardingTable.cpp:301 InstallMapping | correlation forwarding 记录 | Z:zForwarding.inline.hpp:267 insert 的 CAS 本体保留 |
| Heap/z/zMark.cpp:64 / :219 / :749 / :1572 | paint/follow/store 因果记录、header/clear ring 展示、空诊断分支 | Z:zMark.inline.hpp:49 mark_object、Z:zMark.cpp:371 follow_object、:635 follow_work；现有 mark/follow 本体保留 |
| Heap/z/zPage.cpp:178 VisitAllObjects | hole walk-break 记录及 prevObj/prevSize | Z:zPage.inline.hpp:320 object_iterate；步进与停止条件保留 |
| Heap/z/zPage.inline.hpp:1273 ClearUnits；zPageAllocator.hpp 两个调用 | trace/filler 记录、site 参数 | 无 ZGC 独立观测；真实 MemorySet 保留 |
| Heap/z/zPage.inline.hpp:732 / :1583 / :1605 / :1783 / :2274 | clear/phase/stamp 记录 | 无 ZGC 私有相位账；真实标记位、页 owner 与元数据更新保留 |
| Heap/z/zObjectAllocator.cpp:358；zThreadLocalAllocBuffer.cpp:187 | reuse stamp、zap、alloc phase 记录、MinorGCALot | Z:zObjectAllocator.cpp:183 / :238 的分配路径不生产这些诊断；分配本体保留 |
| Heap/z/zRelocate.cpp:374 / :1071 / :2349 | cset census、healing census、trace/filler 记录 | Z:zRelocate.cpp:1289 relocate 与 :838 起 in-place 产品流程；复制/owner/ZeroAndFill 保留 |
| Heap/z/zRelocationSetSelector.cpp:336；zPageAllocator.inline.hpp:54 | cset、trace、garbage 诊断消费 | 无 ZGC 私有记录；页选择/回收本体保留 |
| Heap/z/zRootsIterator.cpp:120 / :210 / :454 | M0 stack-map scope、NwDrop 汇总 | ZVerify 根访问及现有栈图消费者保留 |
| SignalManager.cpp:204 / :289 | heal-pair 与 untag breadcrumb 输出 | 无 ZGC 对应信号诊断；其私有参数采集同删 |
| UnwindStack/StackExposureHook.{h,cpp}、Mutator/MutatorManager.cpp | hook 计数与唯一产品 STW 记录调用 | 自创结构 harness；真实 zStackWatermark 家族保留 |

## 逐文件及测试记录

- `deletion-table.md`：VERIFY_DISPOSITION 的每个原文件及三个附属诊断文件。
- `tests-delta.md`：同一 git 源码尺导出的 GC_TEST/GC_OTHER_VM_TEST 名称集合差；未运行测试。
- 删除六个纯诊断测试 TU 与 stack exposure harness；删除 PeekYoungAllocBlack 专用观测测试。
- 保留 `zgc_self_heal_loop_unit.cpp` 的 CaseUncontended、CaseLostCasThenUpgrade、CaseFastPathExit、CaseNullHealRefused，对应 ZGC self_heal 的四个分支；删除无 ZGC 对应的 CaseSpinAlarm 与 census 断言。未为运行通过改变产品断言或测试输入。
- 保留 test_verify_roots / test_verify_phase / test_verify_marking_stacks / test_verify_fail_close，以及 test_zForwarding.cpp（ZGC 同名 gtest），未改其判据。
- `source-cmake.txt`：每个已删文件名在源码/测试/CMake 作用域的引用检索 rc=1；正针对照 zVerify.cpp 登记 rc=0。
- `grep-evidence.txt`：目标诊断名联合检索 rc=1；保留的 ZVerify/MarkingStacks/屏障检查阳性针 rc=0；冻结 CMake 旧诊断阳性针 rc=0。
- `main-features.txt`：主线与候选同一 git grep -c -F -e 尺，包含原文与 rc。
- `retained-hashes.txt`：zVerify.cpp / zBarrier.cpp / zForwarding.cpp / RefField.h 与合入主线内容摘要相同。

## 构建

只使用 `/root/cj_build/ops/bin/kkk2_build_two.sh`。default/testable 并行两臂，各 `-j$(nproc)`，实际 nproc=192；ulimit -c 0，ccache，GC_UNIT_GATE_SKIP=1。最终源码 SHA 和两端 uptime / SO SHA 在 build-identity.txt；两臂 configure/build rc 及 wall 在 build-summary.log。两构型日志在 kkk2:/root/sym_cangjie_runtime_490_implement_r5654882260/。未运行 unit/gate/切刀，符合本轮 alignment_mode 合同；构建成功不等于行为审查放行。

## 共享工具

Advisor 批准文件：/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_490_implement_r5654882260-20260913T173135Z.md。
按裁定，仅交 `diag_registry.patch`，基于 tools master 87362b2009ee5b9e3bbfd46cb59501c9d0a3b3bc，删除 MRT_GCV2_DIAG 的名册登记。共享文件已有别人未提交修改，本轮未触碰；主控收割时应用该补丁。registry-patch-check.txt 记录基于 master 副本的 git apply --check rc。

## FALSIFIED

初次用泛名 detach_page 计数时主线 12 / 候选 11，减少的是已删 ZgcInvariants.h 的注释，不是页 owner 调用。改用真实调用文本 `->detach_page()`，并使用 `-e` 防止首字符 `-` 被 git grep 当选项，两侧均 3，rc=0。最终原文在 main-features.txt。这纠正的是本棒初次检索口径，不是改产品或调整期望值。
