PROGRESS=DONE · verdict=自适应门已实现且保持默认关闭；可完成负载有得有失，不解锁默认 · LANE=copywork
SIDE_EFFECT: 产品仅改 young copy 的可选 worker gate 与诊断；新增 `MRT_GCV2_EVACPAR_WORK_GATE=1` 才生效，默认路径仍把原有 pool 及其全部配置 worker 原样传入 copy；未改 `evac_finish`、三门、SATB、驻留合同或共享 SDK。
DELIVERY_REF=cangjie_runtime|fix/copywork|f27e8ddcaa8595360939414f57bd7982a7610363

# copywork：young copy 的 bytes/workers 自适应门

## 1. 结论

本棒完成了 `REPORT-evacpar.md` §8 留下的实作项：young copy 现在可以直接用已经统计的 from-space bytes 决定本 cycle 的 worker 数，并且能实际落到 1 / n / max 三档。门保持默认关。

正式窗为两个 `runtime/tests/gcparity` 负载、四档、每档 N=10，共 80 次；80/80 正常退出且各负载的 stdout checksum 跨档完全一致。结果不是“默认开”的证据：`survival_dense` 有收益，但 `allocation_dense` 的自适应档相对串行在 wall 中位数和 young pause p99 均回退。更关键的是 allocation 的 from-space 约 32 MiB，门因此合理地选到 16 workers，而实际 copy 中位数只有约 1.5 ms；`fromSpaceSize` 单一输入无法识别这种“空间厚、活对象 copy 薄”的 cycle。

因此本报告判词为 **GATE_DONE_DEFAULT_OFF**：实现项闭合，实验入口可用，但不改变默认值，也不主张解锁 `evacpar`。

## 2. 门的判据与落点

产品提交：`f27e8ddcaa8595360939414f57bd7982a7610363`（`perf(gc): gate young copy workers by from-space bytes`）。

落点为 `runtime/src/Heap/Collector/CopyCollector.cpp`：

- 输入只读本函数已有的 `stats.fromSpaceSize = space.FromSpaceSize()`（line 119），没有在别处重新测量。
- 门开关为 `MRT_GCV2_EVACPAR_WORK_GATE=1`（lines 129-130）；未设置或不等于 `1` 即关闭。
- `bytesPerWorker = 1 MiB`（line 142）。门开且未 force-serial 时：

  ```text
  workers = min(maxWorkers, max(floor(fromSpaceSize / 1 MiB), 1))
  ```

  对可分配出的每个 worker 保证至少 1 MiB from-space；不足 1 MiB 时仍保留执行所必需的 1 worker。
- `workers == 1` 走既有串行调用；`1 < workers < maxWorkers` 时只为本次 copy 调整 dedicated pool 的 active helpers；完成 `space.ForwardFromSpace()` 后恢复原 active helper 数（lines 143-166）。没有把这个限制带到其他 phase。
- 既有诊断扩为：

  ```text
  [GCV2][evacpar][copy] parallel=%u workers=%d bytes=%zu bytesPerWorker=%zu maxWorkers=%d pool=%s forceSerial=%u workGate=%u
  ```

  所以每个 young cycle 同时留下实际 workers、判据 bytes 和上限。

门形状的正控使用默认关闭的 `MRT_GCV2_MINOR_GC_ALOT=32`，只验证路由，不进入正式性能数字。在最终实现的 allocation 运行中记录到 `workers=1` 47 次、`workers=2` 131 次、`workers=16` 1 次；例如约 1.2--2.0 MiB 落到 1，2.0--2.8 MiB 落到 2，常规约 32 MiB 落到 max=16。正式 auto 档的全部 187 个 gate event 也逐条验证了上述公式，没有错配。

## 3. 默认行为不变

对照对象：改门前 `9bc72903517fd643d2e1c9cab18aa35aa31d7e75` 的 Release/COPYGC `.so`，以及同一源码树上加入本门后的 Release/COPYGC `.so`。门均未设置；负载使用与正式窗相同的官方 cjc 编译产物。

| 负载 | 改门前 | 改门后、gate unset | 确定性输出 | young cycles | 实际 copy 路由 |
|---|---:|---:|---|---:|---|
| allocation_dense | RC=0 | RC=0 | stdout SHA 均为 `edf1f7e7...25c82` | 8 / 8 | 均为 `pool=shared`、2 workers；新诊断 `workGate=0` |
| survival_dense | RC=0 | RC=0 | stdout SHA 均为 `d13a2e12...53f43` | 11 / 11 | 均为 `pool=shared`、2 workers；新诊断 `workGate=0` |

源码路径上，gate false 不调用 `SetMaxActiveThreadNum()`，仍把原先选中的 shared 或 dedicated pool 原样传给 `ForwardFromSpace()`；差异只有新增的判据计算和允许变化的诊断字段。两份 `.so` 的 `nm -D --defined-only` 名称/类型集合完全一致，没有导出 ABI 变化。

两次对照的 total cycle 分别为 allocation 12/14、survival 24/23；差异来自基于时间的 major 触发在两次独立进程中的漂移，不能伪称 total cycle 逐次相等。默认不变的证据取可复现的 RC、逐字节 stdout、young cycle 数以及实际 pool/worker 路由；门的源码分支进一步证明 gate unset 不改变 copy 执行参数。

对照 `.so` SHA-256：baseline `02fc8e920a3b1b122d96b5912463b9eb8db97adda7c6c3eaf0b871ab7a49f118`，测量 candidate `167ab5da3209c962356e87ad9eeac4d78df9878d29a2571073118b6283fbbc9a`。提交后又从 clean `f27e8ddc` 完整重建，最终 `.so` SHA-256 为 `510694287183ae09dbe5682b4f19cb9329d34de6e15f7f71eade73bb4f61d568`，两负载 smoke 均 RC=0、stdout checksum 符合预期，诊断路由符合公式。

## 4. 正式测量方法

负载只使用：

- `runtime/tests/gcparity/allocation_dense.cj`
- `runtime/tests/gcparity/survival_dense.cj`

二者由本机只读的官方 Cangjie SDK 1.1.3 `cjc` 编译，使用 `--static-std`，不加优化；`cjEnableGC=0` 仅用于 compiler host。没有使用共享 SDK，也没有使用 `packages/basic`。后者已知 0/80 无法完成，崩溃前 wall 不构成性能证据。

统一运行条件：256 MiB heap、`MRT_GCV2_FULL_YOUNG_SCAN=0`、`MRT_GCV2_MARKPAR_FORCE_SERIAL=1`、CPU 0-15、单次 timeout 120 s、`MRT_REPORT`/`MRT_GC_LOG=1`。四档轮转执行，避免把整段时间漂移固定压到某一档：

| 档 | 配置 | 用途 |
|---|---|---|
| S1 | `MRT_GCV2_EVACPAR_FORCE_SERIAL=1` | 1 worker 基线 |
| P4 | `MRT_GCV2_EVACPAR_WORKERS=4`，gate off | n=4 envelope |
| P16 | `MRT_GCV2_EVACPAR_WORKERS=16`，gate off | max=16 envelope |
| A16 | workers=16，`MRT_GCV2_EVACPAR_WORK_GATE=1` | 实际 bytes/workers 门 |

每负载每档 N=10。wall p99 对 10 个进程 wall 取 nearest-rank；young pause 与 copy 的 med/p99 对该档所有 young-cycle 事件合并统计。`cycles` 为 total GC cycles 的每进程中位数及 `[min,max]`，并另列 young/minor 与 major 中位数。正式窗没有设置 `MRT_GCV2_MINOR_GC_ALOT`。

## 5. allocation_dense：N=10/档，40/40 完成

所有档 stdout SHA-256 均为 `edf1f7e7cae39e9e97fd3093a5206bf8d4b4800344208d45003702c383d25c82`。

| 档 | 完成 | wall med / p99 (s) | young pause events · med / p99 (us) | copy med / p99 (us) | cycles med [min,max] | young med [min,max] · major med | worker histogram |
|---|---:|---:|---:|---:|---:|---:|---|
| S1 | 10/10 | 0.925 / 1.27 | 75 · 5,560 / 13,296 | 1,544 / 3,506 | 13 [12,14] | 7.5 [6,9] · 6 | 1:75 |
| P4 | 10/10 | 0.870 / 1.17 | 73 · 6,009 / 12,766 | 1,570 / 2,516 | 13 [12,14] | 7.5 [6,8] · 5 | 4:73 |
| P16 | 10/10 | 0.925 / 1.54 | 74 · 6,802 / 15,285 | 1,880.5 / 4,902 | 13 [12,16] | 7.5 [6,8] · 6 | 16:74 |
| A16 | 10/10 | 0.940 / 1.32 | 76 · 6,643.5 / 14,018 | 1,941 / 9,393 | 13.5 [12,15] | 7.5 [7,9] · 5.5 | 16:76 |

A16 相对 S1：wall med `+1.62%`，young pause p99 `+5.43%`。76/76 个 cycle 都因约 32 MiB from-space 选择 max=16；这里自适应门没有识别出实际 live copy 很薄，结果没有性能收益。

## 6. survival_dense：N=10/档，40/40 完成

所有档 stdout SHA-256 均为 `d13a2e124ceea74238595cafe2457fe00f35989ffd0b6b22256877744ce53f43`。

| 档 | 完成 | wall med / p99 (s) | young pause events · med / p99 (us) | copy med / p99 (us) | cycles med [min,max] | young med [min,max] · major med | worker histogram |
|---|---:|---:|---:|---:|---:|---:|---|
| S1 | 10/10 | 14.380 / 24.78 | 112 · 961,665 / 2,273,764 | 153,251.5 / 377,325 | 23 [23,25] | 11 [11,12] · 12 | 1:112 |
| P4 | 10/10 | 12.685 / 23.26 | 114 · 828,375.5 / 2,025,588 | 46,562.5 / 113,761 | 23 [23,25] | 11 [11,12] · 12 | 4:114 |
| P16 | 10/10 | 11.700 / 17.36 | 110 · 794,897.5 / 1,484,847 | 20,018 / 39,888 | 23 [23,23] | 11 [11,11] · 12 | 16:110 |
| A16 | 10/10 | 13.560 / 16.34 | 111 · 876,443 / 1,450,435 | 22,810 / 49,538 | 23 [23,24] | 11 [11,12] · 12 | 1:1, 16:110 |

A16 相对 S1：wall med `-5.70%`，young pause p99 `-36.21%`。111 个 cycle 中一个薄 cycle 选到 1，其他 110 个选到 max=16。它证明门能在正式可完成负载上改变决策并保留明显收益，但 wall med 仍弱于固定 P16 的 `-18.64%`；结合 allocation 的回退，不能据此默认开启。

## 7. 与旧 `REPORT-evacgate.md` 的差别

旧报告运行在 2026-08-12 runtime 历史重写前的废弃提交 `0280bc7b`/`6beebdaf`，其 RC、wall、阈值扫描等数字一律没有带入本报告。本棒基于当前 `9bc72903`/`f27e8ddc`，使用 `e7d22140` 后已经合入的 phase-local dedicated pool，并重新构建、重新跑完 80 次正式窗。

设计上也不是旧式“低于阈值便串行”的二值门：本棒按 `floor(bytes / 1 MiB)` 连续计算 worker 数，再 clamp 到 `[1,maxWorkers]`，所以存在 1 / n / max 三档。旧报告仅提供两个方法性提醒：如何做阈值/档位对照，以及必须防止“阈值看似命中但没有搬移正计数”的假证据。本棒因此用逐 cycle 的 bytes/workers 诊断、copy 事件数、公式逐条校验和三档正控闭环，不引用旧线的性能结论。

## 8. 没覆盖到什么

- 没有在 `packages/basic` 上给 wall 结论；其正式 `evacpar` 窗 0/80 完成，按要求排除。
- 没有改动或重试 `evac_finish`，也没有改 mark/ref 并发、三门、SATB 或驻留合同。
- 没有默认开启 work gate 或 `evacpar`；判据①的零崩溃尚未闭合，本棒也没有扩大到生产长稳、不同 heap/核数/机器矩阵。
- 1 MiB/worker 只使用题定且现成的 `fromSpaceSize`。它不测实际 live bytes，allocation 的结果已展示这个输入的能力边界；本棒没有额外引入 live-work 预测器。
- 正式窗覆盖 1、固定 n=4、固定 max=16，以及 max=16 下的动态门；自然负载没有持续产生中间动态档。中间动态档由机制正控确认，但不冒充 N=10 性能数据。
- 测量 candidate 是产品提交前、与 `f27e8ddc` 产品 diff 完全相同的构建；提交后的 clean 构建做了 ABI/build/smoke 复核，没有把另一轮短 smoke 冒充正式 N=10。

## 9. 可复核材料

- 正式原始汇总：`/tmp/copywork-evidence/analysis/raw.tsv`，SHA-256 `37ab8094d7ce2a1fd5c8d7617fa969e6d1d161017bb7a3ea4730ff30f638df2a`。
- 表格汇总：`/tmp/copywork-evidence/analysis/summary.tsv`，SHA-256 `c8f7181b88f4ceec5da1829e6940cf90dea5746fdadaf7c139f9a45496fcede9`。
- 正式日志：`/tmp/copywork-evidence/formal/`，共 80 个 run 目录；每个目录保存 env、stdout、stderr、wall、RC 与 GC report。
- 官方 `cjc` SHA-256：`bf2536ea4dbb266ecca660decedad40bdcc373f4090a9eb70ded03bca52b4aae`，version 1.1.3。
- 正式可执行文件 SHA-256：allocation `85a7302cd11f740906e8a8ccb9b9369ebde9996108c3e2b19ee30f5815908d12`；survival `4377c5db2a04fc4cae95ec010e93233a14326e70e5b6af9f29a778c47360ed73`。
- Release/COPYGC clean build 成功；`git diff --check` 通过；最终产品提交作者与提交者均为 `Zxilly <zxilly@outlook.com>`，无 AI 署名。
