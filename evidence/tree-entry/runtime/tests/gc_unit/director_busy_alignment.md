待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# #893：#879 busy 行改判与定向测试

`/root/cj_build/reports/EXPLORE-SD256-0922/summary.md:14` 的原判
“✅ 形态一致（busy 经采样快照，ZGC 直接问 ZDriver；同一判断）”撤回。
在冻结基线 `6fa4ead28f16fc1438983956e459d42c16e79fa3` 上应为
**⚠ 形态不同，(a) bug**。本候选将六处改为各决策点读取真实 port，
并删除 stats/director 的旧 busy 快照链，状态为 **⚠→已修，待独立审查**。
不覆盖他人原报告；此修正记录供主控登记替换该行。

ZGC 根目录：`/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`。
我方根目录：`runtime/src/Heap/z/`。

| ZGC zDirector.cpp | 我方函数/分路 | 来源 |
|---|---|---|
| 608 | make_minor_gc_decision 的 minor 门 | driver_minor()->port().is_busy() |
| 612 | make_minor_gc_decision 的 major/resize 门 | driver_major()->port().is_busy() |
| 632 | make_major_gc_decision 的 major 门 | driver_major()->port().is_busy() |
| 802 | start_minor_gc 的 worker selection | driver_major()->port().is_busy() |
| 807 | start_minor_gc 的 resize 请求门 | driver_major()->port().is_busy() |
| 830 | start_gc 的 major allocation rule 短路门 | driver_major()->port().is_busy() |

两侧 port 的 is_busy/send_async/receive/ack 都持有 ZConditionLock。
ZGC 锚为 zDriverPort.cpp:96/128/138/158；is_busy 表示当前消息状态，
不保证检查与执行原子化。本条不证明 SD256 方差因果。

`run_director_busy_gdb.sh` 在 kkk2 对产品 SO 和既有 gc_unit ELF 运行
36 个独立进程：六处各四个采样/决策状态组合，另有 resize-active、
worker 数相等和真实入口出口对照。它读取实际 is_busy 返回值、实际
后继分路、发送请求的 worker 数和 resize 请求状态；不写 director stats。

```sh
export GC_UNIT_TEST_ELF=/absolute/path/to/cj_gc_unit
export GCV2_RUNTIME_LIB_DIR=/absolute/path/to/matching/product/lib
export DIRECTOR_SOURCE=/absolute/path/to/matching/runtime/src/Heap/z/zDirector.cpp
export BUSY_OUT=/absolute/path/to/new/evidence-directory
export BUSY_CPUSET=0-31 # 使用 cjops windows 实际领取的核域
bash runtime/tests/gc_unit/run_director_busy_gdb.sh
```

单点刀按承重面设置 `BUSY_SITES=major|minor|minor_major|merge|select|resize`；
其中 major/minor/merge/select 各四项，minor_major/resize 各八项。
每臂另跑 `GcDirector.CycleUsesWorkerAccountingAndControlledClock`。
入口刀用 `BUSY_SITES=entry`；生产端刀只用 `BUSY_SITES=entry BUSY_INITIALS=0 BUSY_CURRENTS=0`，
避免故意破坏的 send_async 先影响翻转构造的前置条件。

范围裁决：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_893_implement_r5780611818-20260922T173827Z.md`。
上游门刀使下游夹具无法触达目标的 timeout 不计转红；原始全矩阵结果保留在本棒报告的证据目录。
最终构型、SHA、逐刀断言、三臂差分及恢复结果见
`/root/cj_build/reports/REPORT-sym_cangjie_runtime_893_implement_r5780611818.md`。
