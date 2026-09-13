实现 A12a：指标静态登记到常驻 per-CPU registry，独立 1 Hz 线程将 sampler/counter 归入 10s/10m/10h/总计 history。phase 生产端引用静态身份；分配速率和覆盖的 GCStats 统计本体迁入 ZStat，移除 cycle-end 清表与旧 ZStat 开关。GCLOG 原计时出口保留。

基线：`41b05e57b13c02aff9792588fcb97a4ba5161736`；候选：`1361f2896c47e55ea9b96e76313e7afcc3612972`。

按 advisor 裁定，oldLive 本轮保留基线周期末 used 语义；真实 mark-end livemap 汇总待 A07 后接入，单独登记后续项。

两产品构型 configure/build rc 均为 0，独立目录并行、每臂 -j192。最终 ZStat 测试 TU 编译 rc=0。完整测试目标曾因既有 fixture 的已移除接口而编译 rc=1；日志保留。按 alignment_mode 不运行 unit/切刀/nwdet，不作行为验收通过结论。

候选边界逐笔提交：

|提交|机制|
|---|---|
|`296e4ffd7`|feat(gc): port resident statistics registry and history (zStat.cpp:386,1036)|
|`08c97df1e`|refactor(gc): remove ZStat compile gate (zStat.cpp:1029)|
|`02f5ca18f`|fix(gc): align permanent statistics storage in C++14 (zUtils.inline.hpp:37)|
|`4b5ca400b`|fix(gc): retain scoped heap statistic semantics (zStat.cpp:1788)|
|`8084195d2`|test(gc): bind statistics tests to resident product registry (zStat.cpp:386)|
|`1361f2896`|fix(gc): bind forwarding phase identity to its generation (zStat.cpp:805)|

首笔包含 registry/history 及其统计生产消费迁移，结论只对整体成立。

六栏验收卡：

|基线身份及摘要独立复算|真实产品调用链|测试与产品归属|产物身份|定向切刀|测试集合差|
|---|---|---|---|---|---|
|rev-parse rc=0；commit payload SHA256 与 Git object SHA1 重算见 identity.txt|Timer/allocator/driver → 静态 sampler/counter → tick → history → Print，逐层锚在报告|测试构建入口已删除 ZStat.cpp 重复编译；产品 full nm --defined-only 证明两构型在场|两构型 runtime/boundscheck SHA256 与 CJRT-COMMIT 见 artifacts.json；测试 TU 对象摘要已记录，未生成测试 ELF|对齐期明确不运行，不宣称行为通过|ZStat 新增5、替换4、保留2；逐名原因与集合差见报告|

报告：`/root/cj_build/reports/REPORT-sym_cangjie_runtime_465_implement_r5651041991.md`

证据：`/root/cj_build/reports/EVIDENCE-sym_cangjie_runtime_465_implement_r5651041991`；远端 `kkk2:/root/sym_cangjie_runtime_465_implement_r5651041991`。

Refs #465。
