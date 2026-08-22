# LEAD-NOTE-3（0822 18:1x 主控写 —— ⭐ 有人已经把"谁没被 walk 覆盖"这把尺造好了）

`REPORT-remapdomain.md` 刚交付阶段①，它做出来的东西正是你族 B 需要的：

- ✅ **`DeferredRemapDomain` 影子域 + postflip oracle 对账**已上树：
  gc_unit **272/272**；sd256 审计 N=8 **8/8 OK**；**28 轮 parity 全 `missing=0 extra=0 duplicate=0`**。
- ⇒ ⭐⭐⭐ **那个 oracle 算的就是「postflip walk 覆盖了什么 / 漏了什么」** ——
  而你族 B 的审计点（`InvalidateOldTaggedRefsBeforeDispel` / `walkRange` `:2627` 起的 **holder 全集定义**）
  问的正是同一件事。
- ⚠ 它在 nw256 级有代价与噪声：影子开销 **~10× 产品墙**，且**存在捕获窗口 miss**（最差单轮 641/46425）
  ⇒ 主控已派 `remapdomain2` 去把 miss 分成「尺的问题」还是「真缺陷」，
  并把 oracle 的输出从**计数**改成**点名**（holder 地址 / region 类型状态租期 / 字段原始字与色位 /
  为什么不在 walk 集合里）。

## 给你的两条

1. ⭐⭐ **别重复造尺**：如果你在族 B 那条对照臂里需要"哪些 holder 没被 walk 覆盖"，
   先看 `lane/remapdomain` 的影子域能不能直接用（它已经过 gc_unit 272/272 与 28 轮 parity）。
   ⇒ 两条棒的结论要能互相对上：**你点名的族 B holder，应该出现在它的 missing 明细里**。
2. ⭐ **复现器已统一**：remapdomain2 也被要求改用 heudefer 的配方
   （`survival_dense` + `cjGCInterval=5s` + `cjHeapSize=256MB` + `MRT_GCV2_MARKPAR_FORCE_SERIAL=1`，
   连续 N=8 漂 8 次）⇒ ⭐⭐ **三条棒（你 / heudefer / remapdomain）现在跑同一个复现器**，
   证据可以直接叠加。⛔ 别再各用各的负载。

⭐ 主控最想要的一格（remapdomain2 也在做，谁先做出来都算）：
**在漂的那一发上，oracle 报不报 missing；不漂的那一发报不报。**
⇒ 漂发 missing>0 且不漂发 missing=0 ⇒ **族 B 当场坐实**。

⛔ 边界不变：不碰 heudefer 的 probe 装置、promoedge 的写屏障面、lockstate、gclive 的栈图契约。
