# LEAD-NOTE-2（0822 17:5x 主控写 —— ⭐ 独立第二条棒把你的族 B 顶实了，且**证伪**了我上一条note里的一半假说）

`REPORT-heudefer3.md`（独立装置、独立复现器）结论：

- **判定 = 乙′**：`src=from` **3/3**（`LookupTo(last)` 与 `FindToVersion(last)` **都不等于** `cur`）；
  **`sameRegion=1`**；**`life` 相同（都是 1）⇒ 不是跨租期**；`receipt ≠ cur`。
- ⭐⭐⭐ **构造性对照臂**：新增 `MRT_GCV2_KILL_STALE_ROUTE=1`（掐掉 retired 表 + 几何路由）
  ⇒ **仍漂（1/4 金，未归零）** ⇒ **甲′「上一租期路由被沿用」证伪**。
  ⇒ ⭐ 我在 LEAD-NOTE-1 里提的「经过上一租期的路由/转发信息」那半 **不成立**，⛔ 别再按它查。
- ⭐⭐ **它自判落在你的族 B**（256MB · `RECENT_FULL` · 0x40 步长对象 · **字段带旧色**），⛔ 不是族 A
  （族 A 的指纹是 128MB 单对象 `allocPtr=+0x30`）。
- 直接支持族 B 的两个样本：
  - `r01`：**槽当前字带旧色**（`curOld=1`，`loadBad=0xd000000000000`）；
  - `r03`：`last` **hdr=0 且地址 > allocPtr** ⇒ **正是你记的 `hole_above_alloc`**，且 `lastOld=1`。

## 对你（staleclr）的含义

⭐⭐ **两条独立的线现在都指向族 B（heal 覆盖缺口）**，且都不是"路由/转发"问题。
⇒ 建议把本轮的重心压到**族 B 的那条对照臂**上：
把 postflip walk 的 **holder 全集扩到包含 allocating-page / 判死 holder**（诊断开关，默认关）
⇒ 若族 B 样本归零 ⇒ B 成立、且**修法位置就定了**。
⭐ 族 A 那条臂仍要跑（它可能是少数派），⛔ 但别让它占掉主要预算。

## 该立的不变量（两条棒的证据合起来指向同一句）

⭐⭐⭐ **「flip 之后，任何堆字段不得携带早于本周期的色」** ——
ZGC 靠 **load 时 self-heal**（`zBarrier.inline.hpp:318-340`）+ **flip 前对 root/remset 全覆盖 remap**
（`zGeneration.cpp:1408-1523`）保证它；我方两者都不完整。
⇒ 请在报告里明确回答：**我方这条保证断在哪一步**（file:line），以及补它的最小改动是什么。

## 可直接借用的复现器（比 nw256 快且稳）

`cjGCInterval=5s` + `cjHeapSize=256MB` + `MRT_GCV2_MARKPAR_FORCE_SERIAL=1`
（heudefer 实测连续 N=8 漂 8 次；heudefer3 的尺已能在**首个不一致处**打印
槽原始字/旧色位/region 租期/`src` 三分桶——⭐ 那套探针可以直接抄过来）。

⛔ 边界不变：不碰 promoedge（写屏障）· heudefer（probe 装置）· tailslot（几何门）· 编译器侧。
