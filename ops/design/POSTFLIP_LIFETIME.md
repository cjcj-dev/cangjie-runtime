PROGRESS=DONE · verdict=C 保留：当前 forwarding/from-page 身份早于下一次 old mark/remap 闭包丢失，惰性读屏障无法修复 cold field；先改完生命期与原子读路径才能删 134.7 ms · LANE=postflip
SIDE_EFFECT: 纯分析，零产品修法，未占 kkk2 测量窗口，未跑 A/B 验收；sodepot put 复用已实测 a73df6d1f SO，未 push、未改共享 SDK。

# postflip —— 134.7 ms 能否靠读屏障删除

AUTHOR=Zxilly <zxilly@outlook.com>

## 0. 结论先行

选 **C 保留**。不是因为 ZGC 也需要这一遍；ZGC 没有翻转后全堆 adjust。真正阻塞是我方尚未满足 ZGC 惰性修正的生命期前提：

> 前一个 old relocation set 的 `from -> to` 转发答案和 from-page 代际身份，必须一直可解析到下一次 **old** mark/follow + non-strong-reference 处理结束；在此之前，所有可对外交付引用的读路径必须能查询这份答案并 self-heal 原槽。

当前实现在一次 major 后经过少数几次任意 GC（minor 也算）就会依次从 `g_entries` 移除、清 ghost/membership、销毁 retired table；这可以发生在下一次 old mark 之前。而 `relocate_or_remap_object` 又是 **ghost-first**：没 ghost 就直接返回 from，连尚存的 retired table 都不查。因此直接删 walk 后，cold field 可在其首次 load 前失去唯一的 remap 答案。

这一前提可以改；改完后应重新评估 A。但在当前树上，A 不正确，B 也不能靠“只扫 relocation-set 覆盖的区”来保证正确。

## 1. ZGC 靠什么保证

### 1.1 它不保证 flip 后立即“没人拿旧引用”

更准确的不变式是：旧引用可以留在 cold heap field 中，但任何真正使用它的 load 都会先判色；坏色经转发表 remap/relocate，然后 CAS 写回原槽。

- `zBarrier.inline.hpp:295-320`：`barrier` 快路失败后先 `make_load_good`，有具体槽地址 `p` 时调 `self_heal`。
- `zBarrier.inline.hpp:64-98`：`self_heal` 的 CAS 循环；`:82-86` 对 `p` 做 `AtomicAccess::cmpxchg(p, ptr, heal_ptr)`，`:88-97` 在竞态下退出或对新观测值重试。这是“写回槽”，不只是返回新指针。
- `zBarrier.inline.hpp:424-433`：普通 oop field load 将原槽 `p` 传给上述 barrier；weak/phantom 读也在 `:450-507` 进入同一机制。
- `zBarrier.inline.hpp:127-270`：源码注释明确区分 roots/current-remset 的主动 remap 和 heap field 的 lazy self-heal；后者可暂时保留坏色。

上游链接：[`zBarrier.inline.hpp:64-98`](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/gc/z/zBarrier.inline.hpp#L64-L98)、[`zBarrier.inline.hpp:127-270`](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/gc/z/zBarrier.inline.hpp#L127-L270)、[`zBarrier.inline.hpp:295-320`](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/gc/z/zBarrier.inline.hpp#L295-L320)、[`zBarrier.inline.hpp:424-507`](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/gc/z/zBarrier.inline.hpp#L424-L507)。

### 1.2 `ZForwarding` 保留到下一次同代闭包之后

old cycle 的顺序是关键证据：

1. `zGeneration.cpp:947-969`：下一次 old cycle 先做 mark-start/mark/mark-end、mark-free 和 non-strong references。
2. `zGeneration.cpp:972-973`：然后才 `reset_relocation_set()`，重置的是上一轮 relocation set。
3. `zGeneration.cpp:256-265`：reset 逐个从 `_forwarding_table` 移除前一轮 `ZForwarding`，再调 `_relocation_set.reset()`。
4. `zRelocationSet.cpp:177-189`：`ZRelocationSet::reset` 才真正遍历并 destroy 这批 `ZForwarding`。
5. `zGeneration.cpp:980-1001`：之后选新 set，old 在 relocate-start 前先 concurrent remap young roots，再 flip/relocate。

所以上一轮表的生命期覆盖了下一轮同代 mark/follow 与 non-strong processing。冷字段若一直不被 mutator load，就在这个闭包中被读/重标记；之前不会先把解析它所需的表丢掉。`zGeneration.inline.hpp:77-79,123-141` 则显示 remap/relocate 先按地址查 generation forwarding table，不以额外 ghost 标志作为查表前提。

上游链接：[`zGeneration.cpp:256-265`](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/gc/z/zGeneration.cpp#L256-L265)、[`zGeneration.cpp:947-1001`](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/gc/z/zGeneration.cpp#L947-L1001)、[`zRelocationSet.cpp:158-189`](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/gc/z/zRelocationSet.cpp#L158-L189)、[`zGeneration.inline.hpp:77-79`](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/gc/z/zGeneration.inline.hpp#L77-L79)、[`zGeneration.inline.hpp:123-141`](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/gc/z/zGeneration.inline.hpp#L123-L141)。

### 1.3 reset 的精确语义

ZGC 的 `reset_relocation_set` 不是“本轮 flip 后马上清表”，而是“下一轮同代 mark 闭包已经消费完上轮旧引用后，在安装新 set 前清上轮表”。这正是 lazy remap 可成立的时间边界。

## 2. 我方读屏障：有测色与写回，但不足以删 walk

### 2.1 编译器发射与 runtime self-heal

已实证用户看到的指令形状：

- LLVM sibling tree `llvm/lib/CodeGen/CJBarrierLowering.cpp:631-645,680-725`：从槽加载 raw pointer，用 `g_cjLoadBadMask` AND 后分快/慢路。
- 同文件 `:753-790,1271-1307`：好色只保留低 48 bit；坏色保留 `llvm.cj.gcread.ref` / static-ref / atomic-load 慢路。因而“每个经 Cangjie GC read intrinsic 的 load”都测色；不能扩大成“任意 runtime C++ raw load 都测色”。
- `runtime/src/CompilerCalls.cpp:1995-2019`：坏色慢路 `CJ_MCC_ReadRefField` 分流到 `ReadStaticRef` 或 `ReadReference`。
- `runtime/src/Heap/Collector/Collector.h:192-210,226-249`：runtime 用同一 `g_cjLoadBadMask` 定义 load-good，坏色进 `relocate_or_remap_object`。
- `runtime/src/ObjectModel/RefField.h:340-357,381-428`：`HealSlot` 对原 `HeapSlot` 做 compare-exchange；`ZgcSelfHeal` 在 CAS 竞态下继续重试。答案是：**是，普通 heap-field 慢路会写回原槽**。
- `runtime/src/Heap/WCollector/IdleBarrier.cpp:20-77` 和 `ForwardBarrier.cpp:131-215`：坏色路径调 `make_load_good`、转为当前色，再 `ZgcSelfHealLoadGood(field, ...)`；普通读的 load-good 但 stale-from 也有 `ResolveFromCopyForMutator` 补救。

但槽类不统一：`Barrier.cpp:1042-1076` 的 static/root read 可以解析 ghost，却不写回 `ReadOnlyRootSlot`；它依赖 GC root 枚举另行修正。所以“每条 load 都自愈原槽”这个强命题不成立。

### 2.2 cold field 会怎样丢掉答案

我方也有下一次 major mark 修槽的能力：`WCollector.cpp:1461-1595,1695-1730` 的 `TraceRefField` 对 bad/stale target 做 `make_load_good`、`HealSlot`并继续 follow；`TracingCollector.cpp:332-398` 对新标记对象遍历 ref fields。问题是表活不到那一刻。

当前代码的实际时间线：

| 时刻 | 代码事件 | 结果 |
|---|---|---|
| major N 收尾 | `WCollector.cpp:9394-9412` flip、postflip walk，然后 `ExpireKeptFromPreviousCycle` | `RegionManager.cpp:1375-1383` 对新 table 设 `kept_seen_expire` |
| 下一次任意 GC 开头 | `WCollector.cpp:9337-9353` 先 reclaim/expire，若是 minor 随即返回 | `RegionManager.cpp:1375-1378` 见 seen 后 `ClearEntries`；`ForwardingTable.cpp:188-205,265-273` 把 active table 退到 `g_retired` |
| 再下一次任意 GC 开头 | `ForwardingTable.cpp:276-300` retired 变 aged；expire 看不到 active table | `RegionManager.cpp:1385-1388` 进 `ExpireKeptPublish`；`RegionInfo.h:2602-2617,2339-2354` 清 ghost/membership |
| 又一次 GC 开头 | `ForwardingTable.cpp:292-300` | aged table 被 destroy |

这三个边界按“任意 GC”推进，不是按“下一次 old closure 已完成”推进。连续 minor 就足以让 cold old field 在下一次 old mark 之前失去 ghost 和 table。

还有更直接的查询次序问题：`WCollector.h:323-385` 的 `relocate_or_remap_object` 先 `GetGhostFromRegionAt`，无 ghost/代际不符时在 `:377-385` 直接返回 from；只有通过这个门才在 `:388-418` 查 `ForwardingTable`。虽然 `ForwardingTable.cpp:499-545` 会搜 active + retired 表，这条 product remap 路在 ghost 消失后根本走不到它。

`WCollector.h:663-713` 的 `FindToVersion` 是 table-first，所以 `Barrier.cpp:987-1015` 的普通读 stale guard 在 retired table 尚活时可以补救。但这不能成为总保证：表最终仍在 old closure 前毁掉，且原子读并未对齐。

### 2.3 原子读是独立缺口

- `IdleBarrier.cpp:87-110`：bad 路径做 `make_load_good` 和 self-heal，但没有普通读 `:50,69` 的 `ResolveFromCopyForMutator`。
- `ForwardBarrier.cpp:238-283`：load-good 快路在 `:244-249` 直接返回 oldTarget；bad 路径也没有普通读 `:161-168,204-212` 的 stale-from 补救。

因此即使 retired table 还在，ghost 被清后 `make_load_good` 也可把 from 重着色写回/交付；快路颜色回绕时则可直接交付 from。这也必须在删 walk 前修齐。

## 3. 非 mutator 路径盘点

| 路径 | 已有保护 | 剩余风险 |
|---|---|---|
| GC 自身 mark/follow | `WCollector.cpp:1461-1595,1695-1730`：bad/stale 解析、HealSlot、follow | 只在下一次 old mark 发生；当前 table/ghost 可更早清除 |
| weak ref | `SatbBuffer.h:352-360` 清理时调 `ReadWeakRef`；`TracingCollector.cpp:367-387` 按 weak 语义跟踪 | 最终仍依赖同一 remap table 生命期 |
| finalizer | `FinalizerProcessor.cpp:22-30,210-226,239-264` 经 `make_load_good` 并 `HealRoot`；`TracingCollector.cpp:1047-1052` 在 GC 枚举三个 finalizer list | `make_load_good` 继承 ghost-first 限制；不是 table 丢失后的独立后备 |
| static/export/JNI-FFI 风格 root | `TracingCollector.h:137-165` export handle 取值经 `ReadStaticRef`；GC 会 visit export roots | `Barrier.cpp:1042-1076` 解析但不 self-heal read-only root slot，正确性依赖 root 枚举时序 |
| raw pointer 边界 | `WCollector.h:500-523` 在 PREFORWARD/FORWARD 对 ghost from 先 resolve 再 pin | 这是显式 pin 契约，不保护任意 cold heap field，也不能替代转发表生命期 |

结论不是“所有非 mutator 路径都已坏”。大部分已经 barriered 或被 root enumeration 覆盖；但它们没有另一套在 forwarding 答案丢失后仍可恢复 from 的机制，故不能用来证明 A。

## 4. 为什么现在不选 B

“只扫 relocation-set 覆盖的 region”不完整：需修的是 **指向** relocation-set/from 对象的 incoming slot，持有者可以在 active heap 任意 region。扫被搬迁区只会找到区内 holder，找不全外部 incoming edges。

dirty/remset 只有在它是“完整 incoming-slot 集合”时才可用：必须覆盖搬迁前已存边、并发写、对象复制产生的边、roots/weak/finalizer/export 等。当前没有这份完整性证明。因而 B 若只是把地址范围换成 relocation-set/dirty 区，是降停顿但不保正确。

## 5. 改掉 C 前提的最小设计方向

1. **按 generation + relocation epoch 延长生命期**：上一 old set 只能在下一 old mark/follow 及 non-strong processing 已完成后 reset，对齐 ZGC `zGeneration.cpp:947-973` + `zRelocationSet.cpp:177-189`；minor 不得推进 old table 的语义年龄。
2. **table-first remap**：`relocate_or_remap_object` 先用转发表自足地回答 from->to，不将 ghost 存在作为查表前提；page incarnation/reuse 必须和 table 的拥有权、读者生命期绑定。
3. **修齐所有 read class**：atomic good/bad 路径与普通读使用同一 stale-from 解析和 CAS self-heal 契约；root/read-only 槽由明确的 remap phase 保证在 flip/reset 前修正。
4. **定义 reset 前的闭包点**：现有 `TraceRefField` 可承担 cold reachable fields 的下一 old closure，但必须先证明 roots/current remset、weak/finalizer 均已消费上轮表，再允许 reset/reuse。

完成这四项后，A 才是结构上与 ZGC 对齐的目标，理论上可消除 HA 实测的 134.740 ms / 17.41%。在那之前，postflip walk 正在补偿转发表太早丢失的协议缺口。

## 6. 与 followconv 观察的关系

`REPORT-followconv.md` 的 current-remset mark-end 全量重走，和本处 postflip 全 active-heap 重走，确实共享一个设计倾向：用 STW census 换掉 ZGC 依赖的跨周期生命期与惰性/并发协议。这是有用的架构线索，但不是本轮 C 的证明；本轮的独立硬证据是 `WCollector.cpp:9337-9353,9394-9412` + `RegionManager.cpp:1348-1397` + `ForwardingTable.cpp:188-205,276-300` 的早退休时间线，以及 `WCollector.h:377-385` 的 ghost-first 短路。

## 7. 验收与交付审计

用户规定的七格验收只在选 A/B 并落修法时触发。本轮选 C，因此没有产品 diff，也没有可声称的前后性能臂。

| 格 | 状态 | 原因 |
|---|---|---|
| HA + SD `ΣSTW` N≥10 同窗 | N/A | C，未删/收窄 phase |
| `rec=phase` 消失/收窄 | N/A | phase 保留 |
| `gc_unit` 删 `.gate_stamp` 双态 | N/A | 无产品修法，未触发 A/B 验收 |
| `sd256` N=8 金值 | N/A | 同上 |
| `nw256` N=20 无新族 | N/A | 同上 |
| wall 前后 | N/A | 同上；未把 134.7 ms 停顿上限冒充为 wall 收益 |
| ELF/trust/SO/核域/uptime | 沿用上轮 HA 证据，本轮无新表 | workload ELF `06ca5b08…eae2`；trust=0 / peel=774；SO `79a86de7…38e5`，stamp `CJRT-COMMIT:a73df6d1…eb1d`；bounds `c3d07ddc…90326`；上轮 cores `80-95/80-95`，uptime `22:52:54→22:53:32` |

SO 交付：本轮零 product change，因此重新 `sodepot put` 的是被分析的原 SO，不是伪造新构建。kkk2 返回 `STORED /root/sodepot/a73df6d1fee52a48f02f20c1c0571f993860eb1d`；runtime SHA-256 `79a86de7fa4bcf948f88f83257cb3585f5fe59aa390af960a15245f1d81038e5`，bounds SHA-256 `c3d07ddc2b3389afcca530982d3f4cd2e7c0ae99d775344d0d55c01182c90326`，SO 内读回 `CJRT-COMMIT:a73df6d1fee52a48f02f20c1c0571f993860eb1d`。

本轮未申请 `cjops windows`：纯静态源码分析与存档核验不占用测量核，因此没有核域/新 uptime 读回。未 push，未改共享 SDK，未改题目列出的禁碰范围；预存未跟踪 `LEAD-NOTE*.md` 保持原样。
