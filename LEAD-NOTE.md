# LEAD-NOTE（0822 17:4x 主控写 —— ⭐ 另一条棒刚交的证据，指纹和你的族 A/B 高度重合）

`REPORT-heudefer2.md`（同日、同战役、独立装置）刚判完它的三选一，结果如下：

- **判定：乙（对象被误回收）**，证据是一把"尺"在**首个 probe 不一致处**读到的：
  - `last`（写槽时的对象地址）**头已是 0 / 垃圾**；
  - `cur`（槽现在指向的对象）是**另一个仍像 `SurvivalNode` 的对象**：**id 邻近**、
    **`type=RECENT_FULL=2`**、**`route=COMPACTED=4`**、`young=0`、`live=0`；
  - **`sameAddr=0`** ⇒ ⛔ 不是"槽没更新"（甲），⛔ 也不是纯非法值（丙）。
- ⭐⭐ 它还把 `MRT_GCV2_FULL_YOUNG_SCAN` 的 getenv 解锁做对了（照 `MarkPartialArray.cpp:21-25` 模板），
  实测 **FYS=1 × `cjGCInterval=5s` × N=4 仍 0/4 金**
  ⇒ ⭐⭐⭐ **「只是 remset 丢了 old→young 边」这个解释被排除**（把 old 拉进闭包照样漂）。

## 为什么这与你的族 A/B 是同一件事（请验证，⛔ 别直接采信）

⭐⭐ 把两边的指纹并排看：
| 面 | 你（f3state） | heudefer2 |
|---|---|---|
| 落区 | **99.99% `RECENT_FULL_REGION`** | `cur.type=RECENT_FULL` |
| 状态 | 目标 region **已回收再生** | `last` 头已清零、`cur.route=COMPACTED` |
| 值的来路 | **字段里的旧色 from 原值**（route=0/receipt=0） | 槽被**重指到另一个对象**（sameAddr=0） |

⇒ ⭐⭐⭐ 一个能同时解释两者的机制假说（**请用你的两条对照臂去证或证伪**）：
**槽里带着旧色 ⇒ remap/heal 时经过了该 region【上一租期】的路由/转发信息 ⇒ 被解析到一个错误但"合法"的对象**。
- 若成立 ⇒ 你的**族 B（heal 覆盖缺口）**与**族 A（复用单元带旧位）**其实是同一条链的两端；
  heudefer2 看到的"重指到邻近 id 对象"就是**旧路由被复用租期沿用**的直接后果。
- 若不成立（例如你的样本里 route=0 从不参与解析）⇒ 请明说，并给出两族为什么表象相同却机制不同。

## 建议你多记两项（很便宜，能直接判上面那条）

1. 坏样本所在 region 的**租期序号**（第几次再生）与**上一租期的路由/转发表是否仍可达**；
2. 该 holder 的字段值在**上一次 flip 前后**是否被 heal 覆盖过（覆盖过 ⇒ 偏族 A；从没覆盖 ⇒ 偏族 B）。

⛔ 边界不变：不碰 promoedge 的写屏障面、heudefer 的 probe 装置、tailslot 几何门、编译器侧。
⭐ 复现器可直接借用：`cjGCInterval=5s` + `cjHeapSize=256MB` + `MRT_GCV2_MARKPAR_FORCE_SERIAL=1`
（heudefer 实测连续 N=8 漂 8 次），比你现在用的 nw256 更快更稳。
