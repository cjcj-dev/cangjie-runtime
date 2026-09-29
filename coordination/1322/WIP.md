PROGRESS=WIP · verdict=源码接线核对中，运行尺尚未执行 ｜尺=read N=0 · LANE=sym_cangjie_runtime_1322_implement_r5896137675
DELIVERY_REF=none|no-code|尚未改动产品
SIDE_EFFECT: 无
ROLE=implement
EVIDENCE=local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1322_implement_r5896137675/coordination/1322,kkk2:/root/sym_cangjie_runtime_1322_implement_r5896137675/keep
LANE=sym_cangjie_runtime_1322_implement_r5896137675
PROGRESS=WIP

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

冻结坐标回读：`git -C /root/cj_build/cangjie_runtime rev-parse cjcjdev/main`，rc=0，输出 d1f697a053fec85a657286ce5d1c4aeb8fba09b7。

CLAIM: 两项公开统计读取独立的原子全局。
  METHOD: read
  EVIDENCE: runtime/src/CompilerCalls.cpp:473,475；runtime/src/Heap/z/zStat.cpp:1416,1417

## FALSIFIED
自我更正：不能把 ZStatCycle::AtEnd 的预测时长序列直接当作 HotSpot 管理累计时间。实查管理链是 zServiceability.cpp:202 → memoryManager.cpp:222,252 → memoryManager.hpp:161 → management.cpp:838。Issue 的恒零推断尚未运行证实或证伪。

## producer→consumer
当前初始化：zStat.cpp:1416,1417 → CompilerCalls.cpp:473,475；运行期写入待补全核查。ZGC collection 注册结束在 zStat.cpp:667，cycle 结束在 :1242；必须进一步核对管理消费端，不能把 cycle 时长直接当管理累计时间。

## 当前阻塞与已提问
- kkk2 2026-09-30 02:26 实测根盘可用17G，低于构建20G下限；本棒无旧构建可清。未启动构建、未测得当前主线API读数。
- advisor 请求 `/root/cj_build/ops/advisor/inbox/sym_cangjie_runtime_1322_implement_r5896137675-20260929T182710Z.md`：容量与 #1309 RegisterStart/RegisterEnd 同文件交集。
- advisor 请求 `/root/cj_build/ops/advisor/inbox/sym_cangjie_runtime_1322_implement_r5896137675-20260929T182810Z.md`：管理时间生产点前提及最小serviceability移植范围裁决。

CLAIM: ZGC 管理累计时间来自 serviceability cycle tracer，非 ZStatCycle 的预测序列。
  METHOD: read
  EVIDENCE: /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDriver.cpp:173；/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zServiceability.cpp:202；/root/cj_build/reference/jdk/src/hotspot/share/services/memoryManager.cpp:222,252；/root/cj_build/reference/jdk/src/hotspot/share/services/memoryManager.hpp:161；/root/cj_build/reference/jdk/src/hotspot/share/services/management.cpp:838

## 对齐与生产消费表（改产品前）
| 数据/入口 | producer | consumer | 修改位置/形态约束 |
|---|---|---|---|
| 用户GC请求 | stdlib/libs/std/runtime/runtime_gc.cj:40 | runtime/src/CompilerCalls.cpp:445 → runtime/src/HeapManager.inline.h:23 | 必须走公开托管入口，不以组件手工调用替代 |
| 当前累计时间 | runtime/src/Heap/z/zStat.cpp:1416 零初始化 | runtime/src/CompilerCalls.cpp:473 → runtime/src/CommonAlias.h:51 → std.runtime.getGCTime runtime_gc.cj:59 | 新生产端须先裁定与ZGC manager/tracer形态 |
| 当前累计回收量 | runtime/src/Heap/z/zStat.cpp:1417 零初始化 | runtime/src/CompilerCalls.cpp:475 → runtime/src/CommonAlias.h:52 → std.runtime.getGCFreedSize runtime_gc.cj:64 | 不能误用代内反复重置的freed或衰减平均值 |
| 代内回收快照 | zGeneration.cpp:558,1064 → zStat.cpp:848 | zStat.cpp:883 ReclaimedAtRelocateEnd | ZGC zStat.cpp:1818-1842；record_stats控制预测序列，不是总量开关 |
| ZGC累计时间 | zDriver.cpp:173,389 tracer → zServiceability.cpp:202 → memoryManager.cpp:222,252 | memoryManager.hpp:161 → management.cpp:838 | 我方serviceability.hpp:26空类，直接加全局累加不满足形态硬规矩 |

## 装置预检（不计产品验收）
已对现有 sdkdepot/b99430a618af-1ecb811801ca 的静态 std-core 执行标准 std_runtime_colour.py；rc=0，wall=1s，N=1。
证据：kkk2:/root/sym_cangjie_runtime_1322_implement_r5896137675/keep/std-colour.log、std-colour.rc、sdk-uptime-before.txt、sdk-uptime-after.txt；本树 coordination/1322/sdk-evidence 实体副本。
HRT sha256=7d1eeecff4d2a8ed012897785d309a221ddb7520e1b268b431812a0645b56cdc；该旧SDK runtime sha256=37e16b62a38b7b04065e29f5b58f8f4090edc37338ae1792611b07bfc97c90eb。
此预检仅证明旧SDK静态std对声明colour-only集合有U引用；不是冻结基线runtime身份，不证明当前ABI可链接或程序能运行。尚未构造/复制SDK，未改共享安装。
托管测试草稿 coordination/1322/gc_totals.cj 未编译，未算测试通过。

## 产品接线证明
待运行；当前仅静态表，闭环自评 ⚠，不计验收。

## 承重面清单
待完成minor/major、时间/回收量生产与消费切刀。暂无运行证据。

## 测试增删
产品测试集合无变化；只有本棒coordination下草稿。

## 进一步静态核查
- 本树 `coordination/1322/source-evidence.txt` 留下 HEAD、git grep 原命令/结果及各自rc：全局所有出现仅定义/声明/读取；同树搜索管理类名无命中仅作辅助，功能核查以现有serviceability类和driver scope函数体为准。
- 同步返回链：`MCC_InvokeGCImpl`（CompilerCalls.cpp:445）→ `HeapManager::RequestGC`（HeapManager.inline.h:23）→ `Heap::RequestGC`（zHeap.cpp:151）→ `ZCollectedHeap::collect`（zCollectedHeap.cpp:117）→ `ZDriverMajor::collect`（zDriver.cpp:182，USER分支 send_sync）→ `run_thread`（zDriver.cpp:107，gc返回后ack）。托管草稿会在同步返回之后读取产品值。
- #1309 已登记探索报告 `/root/cj_build/reports/EXPLORE-1309-opus-0929.md:48,132` 本身将 serviceability 缺口列为 P15 并排除出其实现范围。若本包采用tracer路径，可减少对其collection日志函数的直接交集；形态及累计回收量消费者仍等advisor。
- 02:32 kkk2 根盘可用继续降至15G；load=173.38/91.37/69.90。未启动构建。
- `cjops deliver check --lane sym_cangjie_runtime_1322_implement_r5896137675` 当前WIP形式检查rc=0；不代表产品验收。
