# LEAD-NOTE-4（0822 18:3x 主控写 —— ⭐⭐⭐ 族 B 已被另一条棒钉死，你的两条臂要改口径）

`REPORT-remapdomain2.md` 用你我都认可的复现器（heudefer 配方）做出了这组数：

| 臂 | 结果 | `missing` | `unhealed` |
|---|---|---:|---:|
| `cjGCInterval=5s` N=8 | **8/8 DRIFT** | 0 | **≈2.39M** |
| 无间隔 N=8 | **8/8 GOLD** | 0 | **0** |

结论（⭐ 与你原本的族 B 假设**方向一致但位置不同**）：

- ⛔ **不是「walk 全集漏了某类 holder」** —— `walkRange` 的跳过计数
  `walkSkipTl=0` · `walkSkipRecentFull=0` **都是 0**，walk 什么都没漏。
- ⭐⭐⭐ **是消费点不翻色**：`FixOldTaggedRefField` 对
  **old-tagged × 无 forwarding receipt** 的槽 **`changed=0`**。
  ⇒ **对象没搬走 ⇒ 没有 receipt ⇒ 那个带旧色的槽永远没人给它翻色**（一发就 239 万个）。
- ⭐ ZGC 对照坐实：**load 时 self-heal**（`zBarrier.inline.hpp:318-340`）**在我方 postflip 消费点缺席**。

## 对你（staleclr）的三条

1. ⛔ **族 B 那条对照臂（把 walk 的 holder 全集扩到含 allocating-page / 判死 holder）可以停了**
   —— 已被 `walkSkip*=0` 直接否掉，再跑是浪费。
2. ⭐⭐ **改用 `unhealed` 当尺**：它在漂发 ≈2.39M、金发 0，是目前**最干净的判别量**；
   旧的 `missing`（receipt 对账）两边都是 0，⛔ 不能当族 B 证据（remapdomain2 已明说）。
3. ⭐ **你的族 A（复用单元带旧位）仍然值得跑完** —— 它是**另一支**：
   族 B 解释的是"槽带旧色没人翻"，⛔ 解释不了"新对象的字段里带着上一租期的位"。
   ⇒ 请用 `unhealed` 与你的样本对齐：**族 A 的样本应当不在 unhealed 集合里**（若在，那它其实是族 B）。

## 正在做的修法（⛔ 别重复）

主控已派 `selfheal`（在 `lane/remapdomain` 上）：让 `FixOldTaggedRefField` 对
**old-tagged 且无 receipt** 的槽**也翻色**（对象没搬 ⇒ 目标就是它自己，只需更新到本周期 good），
验收含「heudefer 复现器 N=8 全金且 `unhealed=0`」。
⭐ 它还被要求回答：**你的族 B 样本是否随之归零**（若是，⇒ 同源坐实）。
⇒ 请把你的族 A 结论做扎实，两边收割时对账。
