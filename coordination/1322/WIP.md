PROGRESS=WIP · verdict=serviceability接线首版已提交，容量阻塞构建，运行尺尚未执行 ｜尺=read N=0 · LANE=sym_cangjie_runtime_1322_implement_r5896137675
DELIVERY_REF=cangjie-runtime|sym/1322-implement-r5896137675|6cea6add341cdccd0df42bf2d4ae36339d7df77e
SIDE_EFFECT: 已推候选 sym/1322-implement-r5896137675；已开草稿PR cangjie-runtime#1366；未动主分支或共享安装。
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

WIP持久化提交 aa54d72430f1d93d5d7f54efafa3c5c3e6ea2e0a，仅coordination；托管草稿未编译，不是产品修复交付。

## 本轮暂停点（保持WIP）
2026-09-30 02:42 再查 kkk2 可用13G，load=39.33/55.54/63.33。等待两份advisor答复的单次 `timeout 600 ...` 调用 rc=124，outbox两份均未到达。未启动低于容量下限的构建。继续需先取得容量/形态裁决；不是测试失败，也不计任何通过。
UNIT_DEFAULT_RC=NOT_RUN(kkk2可用13G低于20G构建下限)
UNIT_FILLER_RC=NOT_RUN(产品尚未构建)
UNIT_OHOS_RC=NOT_RUN(kkk2可用13G低于20G构建下限)
未完成：主线托管实测、产品修复、切刀/恢复、两构型/OHOS/差分、最终接回主线和PR。候选远端仅保存证据提交，不是DONE交付。

## 续轮进展（0930 02:43起，替代上轮等待状态）
已读原任务书 `/root/cj_build/ops/tasks/TASK-sym_cangjie_runtime_1322_implement_r5896137675.md`；工作树干净，HEAD为上轮证据提交；LEAD-NOTE.md不存在。
主控答复已到：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_1322_implement_r5896137675-20260929T182810Z.md`，实体副本 coordination/1322/advisor-authorized.md。
授权最小manager/tracer接线、无ABI变更；getGCFreedSize以HotSpot before/after usage映射累计正差；#1309独占RegisterStart/RegisterEnd，先合后本条接回；<20G先写源码和托管测试。

产品首版提交 `6cea6add341cdccd0df42bf2d4ae36339d7df77e` 已推同分支，**尚未编译**。
- zDriver.cpp:227,244：两个scope持tracer，位置对应ZGC zDriver.cpp:173,389。
- zServiceability.cpp:23,30：manager记录周期前后内存和累计时间；:58,63 scope调用begin/end；:66分别持minor/major manager。
- CompilerCalls.cpp:473,480：既有公开ABI读取两个manager的累计值，不改ABI。
- 删除zStat.cpp/.hpp两个旧全局定义/声明；不改ZStatPhaseCollection::RegisterStart/RegisterEnd。
- 同步保护采用std::mutex，查询累计完成值；before/after正差为主控明确的Cangjie累计API映射，不能宣称HotSpot本身有累计回收量全局。并发分配可使单周期正差为0；测试构造弃用对象后显式GC。

CLAIM: 候选删除旧全局，公开统计读取周期manager；这是源码事实，尚无运行结论。
  METHOD: read
  EVIDENCE: runtime/src/Heap/z/zServiceability.cpp:23,30,43,49；runtime/src/Heap/z/zDriver.cpp:227,244；runtime/src/CompilerCalls.cpp:476,483

### 删除清单
`git grep -n <符号> d1f697a053fec85a657286ce5d1c4aeb8fba09b7 -- runtime/src`：g_gcTotalTimeUs与g_gcCollectedTotalBytes各3处（声明/定义/读取），rc=0。
`git grep -c <符号> HEAD -- runtime/src`：两符号均无输出、rc=1（0命中）。无别名或stub。

### 测试增删补充
新增 runtime/tests/gc_unit/gc_totals.cj 与 run_gc_totals.sh；原测试无删除/弱化。
测试通过std.runtime读取两值，3次显式major GC后逐项打印TARGET并判断>0及单调；两断言均先求值再返回，避免一项遮蔽另一项。草稿源与runner均未编译/运行。
仍须补minor实际运行覆盖、baseline/candidate/producer-cut/consumer-cut/restored身份一致证据；不得以源码推定闭环。

### 续轮验证状态
`git diff --check` rc=0，仅空白检查。02:45 kkk2仍可用13G，load=56.55/58.06/62.63。所有构建、产品测试、切刀、OHOS、差分均未运行。
未接回主线，待#1309先合及容量恢复后完成接回+构建+全部验收。未送Review，PR为draft。
SYM-PR: cangjie-runtime#1366
