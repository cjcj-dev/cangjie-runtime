待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# A12b：serviceability、backing 与 collection identity

派发基线 `78fc9ce028de705b3ea705b7069759c1036a2796`；合入前置包后的坐标 `403916767341fd0610fdc7a1201f1a0333e0da38`。
ZGC 根为 `/root/cj_build/reference/jdk/src/hotspot/share/gc/`；我方下表路径相对 `runtime/src/`。

| ZGC 函数/类型 | 我方函数/类型 | 本包形态 |
|---|---|---|
| z/zServiceability.cpp:40 compute_memory_usage_info | Heap/z/zServiceability.cpp:10 ComputeMemoryUsageInfo | old current=min(old used,capacity)，young current=剩余；两代 max 均为 heap max |
| z/zHeap.cpp:120 used_generation；z/zServiceability.cpp:142 get_memory_usage | Heap/Allocator/RegionSpace.h:75 GetMemoryUsage → Heap/z/zHeap.cpp:239 HeapImpl::GetMemoryUsage | 两代使用页占用；current 读取 committed backing |
| z/zServiceability.cpp:142 get_memory_usage | Inspector/ProfilerAgentImpl.cpp:100 GetHeapUsage | 真实接口输出双代 usedSize/currentSize/maxSize；uint64_t 数字写入 |
| shared/gcId.cpp:50,79,83,87 | Heap/z/zGCIdPrinter.hpp:13 GCIdMark | 唯一递增 ID，线程上下文与嵌套恢复 |
| z/zGCIdPrinter.cpp:34,64,68,72,76,80,84,88 | Heap/z/zGCIdPrinter.hpp:35 ZGCIdPrinter、58 ZGCIdMinor、66 ZGCIdMajor | minor/major 分别注册；y/Y/O 标记，未注册为 - |
| shared/workerThread.hpp:44；shared/workerThread.cpp:72 | Heap/z/zWorkers.cpp:119 RunBatch、94 WorkerLoop | 提交者 GCId 随 batch 传入 worker，再恢复线程上下文 |
| z/zDriver.cpp:169,385,416,438；z/zServiceability.cpp:202 | Heap/z/zDriver.cpp:195 ExecuteDriverRequest、460 CopyCollector::RunGarbageCollection | 每请求一条周期；major 子相位共享 ID，子记录保留开始时间/时长与 gc_tag |
| z/zGCIdPrinter.cpp:34 | Base/GcLog.h:58,60,84,100,128 | v4 的所有周期/相位/STW 记录带 gc_tag，单 active seq 删除 |
| z/zStat.cpp:61,89,156 | Heap/z/zStat.cpp:21,30,61 | 三个采样/历史类型定义下沉 TU，公开头仅前向声明 Data |
| z/zStat.cpp:1029,1055 | Heap/z/zStat.cpp:367,377 | 采样/打印内部入口改 TU 局部，产品 Run 仍调用同一算法 |
| z/zPhysicalMemoryManager.cpp:238,244,263,269；z/zNMT.cpp:60,64 | Heap/z/zPhysicalMemoryManager.cpp:207,236 | 基线已按后端 done 更新成功区段；本包保留 |
| z/zNMT.cpp:68,72 | Heap/z/zPhysicalMemoryManager.cpp:179 | 不添加完整 map/unmap NMT 账；既有映射状态仍用于物理内存管理 |

## producer → consumer

| 顺序 | 生产/消费 | 输出语义 |
|---|---|---|
| 1 | zPage.cpp:78 SetYoungRegionFlag / :101 GetYoungAllocatedSize；zPageAllocator.inline.hpp:150 GetUsedUnitCount | young 页占用与总页占用；old=总量−min(总量,young) |
| 2 | zPhysicalMemoryManager.cpp:329 GetCommittedSize → zPage.hpp:572 → zPageAllocator.hpp:499 | 读取已提交 backing 容量，保持延后发布流程 |
| 3 | RegionSpace.h:75 → zServiceability.cpp:10 → zHeap.cpp:239 | old 优先分配 current、young 使用余量，两代 max 不做容量分割 |
| 4 | ProfilerAgentImpl.cpp:110-134 → FileStream.cpp:84 WriteChunk | 生成 result 的总量及 young/old；stream 追加 profiler 字段并闭合外层 |
| 1 | zDriver.cpp:208 GCIdMark | 一次请求只创建一个 collection ID |
| 2 | zDriver.cpp:241/271/275 注册 Y/y/O | major preclean/roots/old 共享 ID；minor 独立 ID |
| 3 | zWorkers.cpp:119 → :94 | worker 使用对应提交者 ID |
| 4 | Base/LogFile.h Timer → Base/GcLog.h；SignalManager.cpp CurrentSeq | 消费执行线程 ID，不从全局 active 槽猜归属 |
| 5 | zDriver.cpp:287 Cycle / :513 Phase → tests/perf_vs_official/gclog_schema.py | 唯一周期与子相位通过 seq 连接，保留 gc_tag 和时间 |

## 删除与替换

- `Base/GcLog.h` 基线 :59 BeginCycle、:70 CompleteCycle、:276 CycleCounter、:282 ActiveSeq 删除，使用 GCIdMark / ZGCIdPrinter。
- `Inspector/ProfilerAgentImpl.cpp` 基线 :113-115 totalSize/max 混用删除，替换为显式 currentSize/maxSize 和两代字段。
- `Heap/z/zStat.hpp` 基线 :82/:91/:122 的三个完整类型删除，定义迁入产品 cpp；原公开 ZStat::SampleAndCollect/Print 删除，调用改 TU 内部。
- 原 per-generation Cycle 发射移到 driver 请求边界；子阶段以 Phase 发射。
- Windows 旧 CycleCounter 导出清理见最终删除证据。
- 原文与 rc：`evidence/a12b/deletions.txt`；相同检索在基线的命中作为阳性对照，JSON 保存 argv，避免 shell 管道歧义。

## 测试同批迁移

ZGC 参考树没有专门的 zStat / zServiceability / zGCIdPrinter gtest 名称；对应的是上表产品函数，不能把本地测试名冒记成上游测试。

按 advisor `20260913T211044Z`，删除直接构造 TU 私有采样类型的六项：CriticalPhaseRecordsDurationAndFrequency、PauseAndConcurrentKeepStaticIdentity、ZeroDurationSampleStillCounts、CounterTickConsumesAndRetainsHistory、HistoryRollsOverAllThreeLevels、HistoryIncludesPartialIntervals。RegistrySortedWithoutChangingIdentity、RegistryExistsBeforeSampling、StwDepthCounterClassifies 保留；RegistryExistsBeforeSampling 只保留公开 registry 的断言。

新增 ZServiceability.GenerationCapacityAndLimit、ConcurrentSamplesClampToCapacity、CapacityBeyondSigned32Bits，以及 ZGCIdPrinter.MinorAndMajorCoexist、WorkerScopeRestoresThreadContext。timer_ledger_contract 使用 GCIdMark。GCLOG reader 与同目录关联 fixture 同批迁为 v4：未知版本/非法数字/缺字段/深度与路径等既有拒绝条件保留，增加 gc_tag 枚举、缺失 tag 和 v3 拒绝用例。完整集合差见 `evidence/a12b/test-set-diff.json`。

形态对齐轮不运行 unit/gate/切刀；测试源码迁移与语法检查不能称为测试通过。

## 接口与后续项

- Heap usage 的 current 为 committed 页容量，used 统一为页占用；从原对象分配字节与 max-only totalSize 改为上述语义。`ProfilerAgentImpl.cpp` 是现有 OHOS 条件路径；规定的 Linux 双构型构建不证明 OHOS 序列化已运行。
- GCLOG 周期/相位/STW 为 v4，独立异常记录维持 v3；单个 major 的 gc_tag=Y 子相位可以重复，但只有一个周期记录。
- 跨仓只读坐标：cjcj `52cb48f5524f67f4df79761bac863441d2b72c79`，tools `2c5e3566491f97529149009cdf65b7d6b41f882d`。扫描 `.cj/.py/.ts/.cpp` 的 getHeapUsage/totalSize 与 GCLOG 消费入口，cjcj 命中为 DIBuilder 不相关同名局部变量；tools 命中为注释 fixture。本包未修改跨仓接口。
- 独立 director 采样单位问题已开 cangjie-runtime#507：`zStat.cpp:180-182` 仍用对象字节减 young 页占用。本包 serviceability 总 used 使用页占用，未扩大到 director 策略。

## FALSIFIED

没有据此关闭本 issue。成功区段记账已在基线，不能因旧 MemMap 文件路径消失而判机制缺失。实施中的定点删除核对曾未涵盖 Windows 导出；扩至整个 runtime/src 后发现唯一旧 CycleCounter 导出，按专门范围裁定处理，前后检索均保留。

## 最终构建证据

源码提交 `1eb74ee938bfd4b8a6f4b84cc68cec760eb8e8c8`，runtime tree `0fb6926f310ef29f749e484590d6e9d4b9485f92`。
两次交付前 fetch/merge 均读到主线 `403916767341fd0610fdc7a1201f1a0333e0da38`。

| 构型 | CMake | configure_rc | build_rc | wall | runtime SO sha256 |
|---|---|---|---|---|---|
| default | `-DMRT_TESTABLE_INTERNALS=OFF` | 0 | 0 | 48s | `6cba846d8ef4562bea3a0da74df46628835a92bc9318112132938e4dd40abc79` |
| testable | `-DMRT_TESTABLE_INTERNALS=ON` | 0 | 0 | 48s | `1cd0455cc45f141a65c29f845f86928798be088754051fd630f1c5ec94fc26dd` |

两臂 boundscheck SO 均为 `f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883`。
唯一配方 `/root/cj_build/ops/bin/kkk2_build_two.sh`，两臂并行、实读 nproc=192。
构建前后 kkk2 uptime/load 与构建时捕获的 SO 摘要在 `evidence/a12b/artifact-metadata.json`。
日志保留 `kkk2:/root/sym_cangjie_runtime_495_implement_r5656147173/{default,testable}-{configure,build}.log`。
5 个变动 Python 文件仅做语法编译，rc=0，未执行测试。`nm --defined-only` rc=0，并保留整表于同一 kkk2 根下 `default-nm-defined.txt`/`testable-nm-defined.txt`；匹配结果证明新容量计算与线程 ID 上下文在产品 SO 中。
