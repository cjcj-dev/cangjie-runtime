待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`
LANE=sym_cangjie_runtime_1273_implement_r5882118270
ROLE=implement
坐标 279a021724838c0d90580bc5251040972be42f4f；冻结回读 rc=0。
需裁决：1. Heap::is_old(zHeap.cpp:537-540) 自身也是 page != nullptr && !IsYoungRegion；只在 ZBarrier::remember 改用它仍静默跳过。ZGC zHeap.inline.hpp:44-57 是 is_young 直接解引用页、is_old 取反。拟一并按该形态改 Heap::is_young/is_old，并补 ZGenerationYoung::remember 委托 remembered（ZGC zGeneration.inline.hpp:158-159），确认范围。
2. INVESTIGATE I1 要合法 old 页槽经真实写入后记忆集缺失。当前 remember 对所有合法 old/young 页与修后分路真值相同；唯一行为差是无页表项。故按本条修法恢复旧判断不可能使合法 old→young 构造 I1 转红。不能为凑红改四个已对齐写入函数。请求明确本条验收：合法 old/young 产品入口保持记忆集结果；无页表项产品入口不静默成功作为精确反向对照（子进程观察异常终止），是否替代原 I1/I3？否则请给可区分此处分路且满足 I1 的构造。
产品尚未改动；等待期间继续只读调用链与夹具核查。

## producer → consumer（改产品前登记）
| 顺序 | 产品位置 | 数据与责任 |
|---|---|---|
| 1 | runtime/src/CompilerCalls.cpp:91-93 | CJ_MCC_StoreBarrierOnHeapFieldNoKeepAlive 接收编译器传入槽地址，调用 runtime 屏障 |
| 2 | runtime/src/Heap/z/zBarrierSet.cpp:98-100 | 调用 no_keep_alive_store_barrier_on_heap_oop_field |
| 3 | runtime/src/Heap/z/zBarrier.inline.hpp:286-292 | 读取原槽值，store-good 快路或 no_keep_alive_heap_store_slow_path |
| 4 | runtime/src/Heap/z/zBarrier.cpp:193-196 | 消费槽地址，调用 remember(p) |
| 5 | runtime/src/Heap/z/zBarrier.inline.hpp:379-386 | 本条修改分路：old 判定，记忆集委托；必须发生在 page->remember 前 |
| 6 | runtime/src/Heap/z/zRemembered.inline.hpp:17-21 | ZGC 对应的记忆集消费者，取页并 remember |
| 7 | runtime/src/Heap/z/zPage.inline.hpp:279 | 最终页记忆集写入；测试读取 is_remembered，禁止修改该写入函数 |

拟用同一 ELF 调用步骤1真实导出入口。合法 old/young 输入观察槽记忆集；缺页输入在子进程中从 page_table remove 一个已有分配页，槽物理存储保持可读，进入步骤1，观察是否静默返回。缺页只为反向对照，不作为合法 old→young 漏记的归因。最终采纳与否等 advisor。
冻结辅助函数直接调用者：zVerify.cpp:201,203,217,219；zRelocate.cpp:220；zRemembered.cpp:240。全部属于堆对象/字段的分代查询，尚未作运行期证明。

## 主控答复（0929 10:0x）
/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_1273_implement_r5882118270-20260929T015350Z.md 批准上述范围与缺页反向对照，撤销本包 INVESTIGATE I1–I3；不修改四写入维护函数。辅助函数调用者核查记录在本棒报告。

## 受控破坏
validation/remember1273/cut-route.diff：仅恢复 remember 原判断与直接页调用，预期只 MissingPageDoesNotReturnSilently 转红，合法 old/young 保持绿；这是候选修改行刀，不能冒充基线已有行刀。
validation/remember1273/cut-entry.diff：断基线已有真实弱屏障相位入口内 slow-path 调用，预期 old 记忆集和缺页拒绝两条转红，young 负对照保持绿。entry_cut_check 已 rc=0；交付用最终 head 再核。
两刀分别在隔离源码副本构建 default SO，同一 default 测试 ELF 运行全部三项；恢复臂使用保存的未切 SO，候选/恢复逐字节相同。
