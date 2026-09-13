from pathlib import Path
import re, subprocess, hashlib
lane='sym_cangjie_runtime_465_implement_r5651041991'
base='41b05e57b13c02aff9792588fcb97a4ba5161736'
head=subprocess.check_output(['git','rev-parse','HEAD']).decode().strip()
root=Path.cwd(); E=f'/root/cj_build/reports/EVIDENCE-{lane}'; Z='/root/cj_build/reference/jdk/src/hotspot/share/gc/z/'
def at(file,needle):
 p=Path(file)
 for i,line in enumerate(p.read_text().splitlines(),1):
  if needle in line:return f'{file}:{i}'
 raise RuntimeError((file,needle))
def R(file,needle):return at('runtime/src/'+file,needle)
def z(needle,file='zStat.cpp'):return at(Z+file,needle)
rows=[
('指标 identity / per-CPU offset',R('Base/ZStat.cpp','ZStatValue::ZStatValue'),z('ZStatValue::ZStatValue')),
('常驻对齐存储',R('Base/ZStat.cpp','void ZStatValue::InitializeStorage'),z('inline uintptr_t ZUtils::alloc_aligned_unfreeable','zUtils.inline.hpp')),
('CPU 数与 CPU id',R('Base/ZStat.cpp','size_t ZStatValue::CpuCount'),z('inline uint32_t ZCPU::count','zCPU.inline.hpp')),
('sampler 构造登记',R('Base/ZStat.cpp','ZStatSampler::ZStatSampler'),z('ZStatSampler::ZStatSampler')),
('counter 构造登记',R('Base/ZStat.cpp','ZStatCounter::ZStatCounter'),z('ZStatCounter::ZStatCounter')),
('sampler CPU 数据初始化',R('Base/ZStat.cpp','void ZStatSampler::Initialize'),z('void ZStatValue::initialize')),
('counter CPU 数据初始化',R('Base/ZStat.cpp','void ZStatCounter::Initialize'),z('void ZStatValue::initialize')),
('原子采样：count/sum/max',R('Base/ZStat.cpp','void ZStatSampler::Sample('),z('void ZStatSample(')),
('collect-and-reset',R('Base/ZStat.cpp','ZStatSamplerData ZStatSampler::CollectAndReset'),z('ZStatSamplerData ZStatSampler::collect_and_reset')),
('counter increment',R('Base/ZStat.cpp','void ZStatCounter::Increment'),z('void ZStatInc(const ZStatCounter&')),
('counter tick sample-and-reset',R('Base/ZStat.cpp','void ZStatCounter::SampleAndReset'),z('void ZStatCounter::sample_and_reset')),
('registry 初始化',R('Base/ZStat.cpp','void ZStat::Initialize'),z('void ZStatValue::initialize')),
('聚合数据 Add/Average',R('Base/ZStat.h','struct ZStatSamplerData'),z('struct ZStatSamplerData')),
('窗口 Add/Total/Accumulated',R('Base/ZStat.h','class ZStatSamplerHistoryInterval'),z('class ZStatSamplerHistoryInterval')),
('三级 history Add/Windows',R('Base/ZStat.h','class ZStatSamplerHistory {'),z('class ZStatSamplerHistory :')),
('phase 构造登记/结束采样',R('Base/ZStat.h','class ZStatPhase {'),z('ZStatPhase::ZStatPhase')),
('Timer 构造与结束生产',R('Base/LogFile.h','explicit Timer(const ZStatPhase&'),z('class ZStatTimer :','zStat.hpp')),
('tick 按 id 汇入 history',R('Base/ZStat.cpp','void ZStat::SampleAndCollect'),z('void ZStat::sample_and_collect')),
('统计线程启动',R('Base/ZStat.cpp','void ZStat::Start'),z('ZStat::ZStat()')),
('1 Hz cadence / history 生命期',R('Base/ZStat.cpp','void ZStat::Run()'),z('void ZStat::run_thread')),
('线程终止 / join',R('Base/ZStat.cpp','void ZStat::Stop'),z('void ZStat::terminate')),
('排序/窗口均值与最大值输出',R('Base/ZStat.cpp','void ZStat::Print'),z('void ZStat::print(')),
('分配率 initialize',R('Base/ZStat.cpp','void ZStatMutatorAllocRate::initialize'),z('void ZStatMutatorAllocRate::initialize')),
('分配率采样粒度',R('Base/ZStat.cpp','void ZStatMutatorAllocRate::update_sampling_granule'),z('void ZStatMutatorAllocRate::update_sampling_granule')),
('分配生产/速率采样',R('Base/ZStat.cpp','void ZStatMutatorAllocRate::sample_allocation'),z('void ZStatMutatorAllocRate::sample_allocation')),
('分配率 stats 消费',R('Base/ZStat.cpp','ZStatMutatorAllocRateStats ZStatMutatorAllocRate::stats'),z('ZStatMutatorAllocRateStats ZStatMutatorAllocRate::stats')),
('已有按代 cycle 数列',R('Base/ZStat.cpp','void ZStatCycle::AtEnd'),z('void ZStatCycle::at_end')),
('generation 时长身份',R('Heap/Collector/CollectorResources.cpp','void CollectorResources::RunCollection'),z('void ZStatPhaseGeneration::register_end')),
('collection 时长身份（含 major prelude）',R('Heap/Collector/CollectorResources.cpp','const uint64_t collectionStart'),z('void ZStatPhaseCollection::register_end')),
('按代 reclaimed 数列/常驻 sampler',R('Base/ZStat.cpp','void ZStatHeap::AtRelocateEnd'),z('void ZStatHeap::at_relocate_end')),
('heap stats 同步快照',R('Base/ZStat.cpp','ZStatHeapStats ZStatHeap::Stats'),z('ZStatHeapStats ZStatHeap::stats')),
('director 单快照消费',R('Base/ZStat.cpp','GcTriggerInputs ZStat::SampleDirectorStats'),z('ZStatCycleStats ZStatCycle::stats')),
]
text=f'''PROGRESS=WIP · verdict=形态迁移与交付整理中；尺=源码图/两构型编译，非行为验收 · LANE={lane}
DELIVERY_REF=cangjie-runtime|sym/465-implement-r5651041991|{head}
SIDE_EFFECT: 常驻 1 Hz 统计线程与 per-CPU 存储；删除旧统计编译/环境开关；未改主分支。
ROLE=implement
EVIDENCE=local:{E},kkk2:/root/{lane}

## ZGC 函数对应表

坐标：基线 `{base}`；候选 `{head}`。下表的 runtime 路径均相对本棒工作树 `{root}`；ZGC 为绝对参考树。命名组织按任务书自定，未新增 MRT_GCV2 开关。此表为实现读证，待独立 Review。

|机制/函数|我方 file:line|ZGC file:line|
|---|---|---|
'''
for name,r,zz in rows:text+=f'|{name}|`{r}`|`{zz}`|\n'
text+=f'''
⚠ **明确未对齐项：oldLive 的数值来源**。`{R('Heap/Collector/CopyCollector.cpp','const size_t liveBytes = young')}` 依 advisor 保留基线 oldLive=周期末 used 标量；ZGC `zStat.cpp:1788-1800` 从 selector 各组 live 累加。我方 mark-end 真实 live 汇总待 A07（livemap 惰性初始化，#464 在飞）合入后接入。不得把本交付表述为该数值语义已对齐。裁定：`/root/cj_build/ops/advisor/outbox/{lane}-20260913T042850Z.md:2`。Young 使用既有标记结果 `gcStats.youngPromotedBytes`；不从 candidate-reclaimed 推算。

平台适配：ZStat 用现有 C++ 线程、条件变量和 LOG 输出承担 ZThread/ZMetronome/LogTarget 的工作；Linux 用实际 CPU id，CPU-id 不可用时按 JDK 平台回退取 CPU 0。输出以 ns/B 等原生单位标注。既有 GCLOG 所需的 STW 深度观察保留在 `ZStat::EnterStwScope/ExitStwScope/WorldStoppedNow`，它不登记身份、不选择 sampler、不参与 history 生命周期；Timer 的 rec=phase/leaf 与 STW 的 rec=stw 出口继续承担原观察合同。

CLAIM: sampler/counter 的 identity 与链表成员关系由构造登记，per-CPU 槽在初始化后常驻
  METHOD: read
  EVIDENCE: {R('Base/ZStat.cpp','ZStatValue::ZStatValue')}; {R('Base/ZStat.cpp','ZStatSampler::ZStatSampler')}; {R('Base/ZStat.cpp','ZStatCounter::ZStatCounter')}; {R('Base/ZStat.cpp','void ZStat::Initialize')}

CLAIM: 统计线程以 1 Hz 消费 counter/sampler，按 id 汇入 10s/10m/10h/总计 history，再按 log level 输出
  METHOD: read
  EVIDENCE: {R('Base/ZStat.cpp','void ZStat::Run()')}; {R('Base/ZStat.cpp','void ZStat::SampleAndCollect')}; {R('Base/ZStat.h','class ZStatSamplerHistory {')}; {R('Base/ZStat.cpp','void ZStat::Print')}

## 静态登记 → 生产器 → tick → history → 输出

变更前顺序表：`local:{E}/producer-consumer.md`。机械检索全文：`local:{E}/producer-chain.txt`。所有下表 phase 生产器都经过 `Base/LogFile.h` 的 Timer → `ZStatPhase::RegisterEnd` → `ZStatSampler::Sample`，之后同走 `ZStat.cpp:282` tick → `ZStat.h:129` history → `ZStat.cpp:292` Print。ZGC 对应是 `zStat.hpp:313` Timer → `zStat.cpp:834` SubPhase register_end / `zStat.cpp:903` DurationSample → `zStat.cpp:1036` tick → `zStat.cpp:171` history → `zStat.cpp:1067` print。

|静态身份（构造锚）|group / 所属域|实际生产调用点|
|---|---|---|
'''
cpp=Path('runtime/src/Base/ZStat.cpp').read_text().splitlines()
allsource=[p for p in Path('runtime/src').rglob('*') if p.suffix in ('.h','.cpp')]
for i,line in enumerate(cpp,1):
 m=re.search(r'const ZStatPhase (\w+)\("([^"]+)", "([^"]+)"\)',line)
 if not m:continue
 sym,group,name=m.groups();calls=[]
 for p in allsource:
  for j,l in enumerate(p.read_text().splitlines(),1):
   if 'ZStatPhases::'+sym in l and p.name!='ZStat.cpp':calls.append(f'`{p}:{j}`')
 text+=f'|`{sym}` / `{name}` (`Base/ZStat.cpp:{i}`)|{group}|'+ '; '.join(calls)+'|\n'
text+=f'''
其它两条真实链：
- allocation：`{R('Heap/Allocator/RegionManager.cpp','ZStatMutatorAllocRate::sample_allocation')}` → `ZStat.cpp` 的 allocation counter 与原 allocation-rate 窗口 → tick/history/Print；director 从 `ZStatMutatorAllocRate::stats` 读取预测快照。
- heap/cycle：`{R('Heap/Collector/CopyCollector.cpp','(young ? ZStat::YoungHeap()')}` → 所属 generation 的 ZStatHeap 数列及 Reclaimed sampler；`CollectorResources::RunCollection` 与 `ExecuteDriverRequest` 分别记录 generation 与整个 collection，不按 phase 名称推代。

## 删除清单

|基线旧机制/路径|替代|
|---|---|
|`Base/ZStat.h` PhaseTotals/Table/RegisteredPhases、`Base/ZStat.cpp` TableLock/CycleTable/NotePhase/NoteCycleEnd、cycle 结束清表|静态 ZStatValue identity、per-CPU sampler/counter、统计线程拥有 history|
|`Base/ZStat.cpp` Enabled/env override/test override；`runtime/config.cmake` MRT_ZSTAT option 与 `runtime/CMakeLists.txt` MRT_ZSTAT_COMPILED 分支|常驻采样；仅既有 LOG level 控制输出|
|`Heap/Collector/MutatorAllocRate.h` 与 `.cpp`，Collector/CMakeLists 中该源登记|本体迁入 `Base/ZStat.h/.cpp`，allocator/director 直接调用 ZStatMutatorAllocRate；无转发兼容类|
|`GcStats.cpp` g_youngDurationSeq/g_oldDurationSeq、g_youngReclaimedSeq/g_oldReclaimedSeq 及其发布 atomics|已有 generation ZStatCycle 数列；ZStatHeap 按代 reclaimed 数列和 sampler|
|`GcStats.h` lastYoung/lastOld/lastGcDuration、warmup/isWarm/isTimeTrustable 重复字段及 CopyCollector 写入|已有常开 ZStatCycle 与其 director 快照|
|测试目标中额外编译的 `Base/ZStat.cpp`|两种测试构建入口都链接产品 SO 中的单一实现|

未删除 GCStats 的既有日志、HEU 行为字段：它们不属于被 ZStat 覆盖的统计本体，本轮不改其它包机制。

候选 `git grep` 零命中原文如下（rc=1 表示匹配不到，不是命令未运行）。基线同命令阳性原文见 `local:{E}/deletions.txt`；其中包括旧构造/调用/分支，不以程序未启动造成的空输出来证明删除。

```text
'''
s=Path('evidence/deletions.txt').read_text()
for block in s.split('\n\n'):
 if block.startswith('$ git grep') and base not in block.splitlines()[0]:text+=block+'\n'
text+='```\n'
text+=f'''
CLAIM: 删除清单中的旧 identity/table、覆盖的统计序列与旧开关符号在候选 runtime/src 精确检索无匹配
  METHOD: measure
  EVIDENCE: local:{E}/deletions.txt（逐命令 stdout 与 rc，候选臂）
  N: 19 个精确符号检索

CLAIM: 同一检索器在冻结基线匹配到上述旧机制定义或调用，构成删除检索的阳性对照
  METHOD: measure
  EVIDENCE: local:{E}/deletions.txt（基线臂，各查询原文）
  N: 19 个精确符号检索

## 双构型构建

配方原文 `local:{E}/remote/build.sh`。两构型同时启动，独立 build/install 目录，各 `-j$(nproc)`，实际 jobs=192，并行臂数=2；CCACHE_DIR=/root/.ccache，C/C++/ASM launcher=ccache，prefix-map 配方原文在脚本。所有步骤 wall 均见对应 rc 文件；未发生 wall>10min 的顺序步骤。

两构型为 `MRT_GC_UNIT_TESTS=OFF` 与 `ON`（后者开启既有 MRT_TESTABLE_INTERNALS），Release、COPYGC_FLAG=1。旧 MRT_ZSTAT 开关已删除，两构型都包含统计产品代码。`GC_UNIT_GATE_SKIP=1`；未执行 unit、切刀、entry_cut_check 或 nwdet，构建后既有 gate 日志明确 `GC_UNIT_GATE_NOT_RUN reason=EXPLICIT_SKIP`。内置 configure 探测只作为构建日志保留，不扩张为统计行为证据。

FINAL_BUILD_RESULTS_PLACEHOLDER

构建过程原样保留：attempt0 两构型 configure rc=1（配方工作目录错，现有 CMake 相对 CJThread 入口找不到 schedule.h）；attempt1 configure rc=0/build rc=1（本轮引入 std::align_val_t，产品 C++14 不支持）；随后改为 ZGC 的 malloc+padding 地址对齐，中间态两构型 build rc=0。最终 rc 与身份以本节表为准。

CLAIM: 两构型编译结果由实际 configure/build 退出码与链接产物摘要记录
  METHOD: measure
  EVIDENCE: local:{E}/remote/default.rc; local:{E}/remote/testable.rc; local:{E}/remote/artifacts.json; local:{E}/remote/provenance-default.txt; local:{E}/remote/provenance-testable.txt
  N: 2 个构型，各 1 次最终构建

CLAIM: 编译器对本轮 C++14 不支持的对齐表达式返回非零，修正前错误诊断被保留，证明编译装置实际执行
  METHOD: measure
  EVIDENCE: local:{E}/remote/attempt1/build-default.log; local:{E}/remote/attempt1/build-testable.log; local:{E}/remote/attempt1/default.rc; local:{E}/remote/attempt1/testable.rc
  N: 2 个构型

CLAIM: 常驻 sampler/counter、SampleAndCollect/Run/history 在两构型产品完整符号表中存在，不以关闭宏代替存在性证明
  METHOD: measure
  EVIDENCE: local:{E}/remote/nm-default.txt; local:{E}/remote/nm-testable.txt; local:{E}/remote/stat-symbols.txt; local:{E}/remote/artifacts.json
  N: 2 枚 runtime SO

## 测试同批迁移 / 集合差

参考树检索范围 `test/hotspot/gtest/gc/z` 与 `test/hotspot/jtreg/gc/z` 中无 ZStat/ZStatistics 专用测试匹配，原文 `local:{E}/reference-tests.txt`。阳性对照同文件先列出实际测试源（例如 test_zArray.cpp、test_zAddress.cpp，目录枚举 rc=0），再做内容查询 rc=1。因此不编造不存在的 ZGC 测试名；本地测试按上述 ZStat 函数不变量新增/替换。

|本地测试|ZGC 对应函数|变更|
|---|---|---|
|RegistryExistsBeforeSampling|ZStatIterableValue 构造/insert，zStat.cpp:386-403|替换 RegistryEnumeratesObservedPhases；静态身份不以观察触发|
|PauseAndConcurrentKeepStaticIdentity|ZStatPhase 构造与 ZStatSample，zStat.cpp:600/883|替换 PauseAndConcurrentAreSeparateAccounts、KindIsSampledAtScopeEntry、MaxPauseTracksLargestPauseSample；两个静态 group + 最大值断言|
|CounterTickConsumesAndRetainsHistory|ZStatCounter::sample_and_reset / ZStat::sample_and_collect|新增 counter→tick→history|
|HistoryRollsOverAllThreeLevels|ZStatSamplerHistory::add|新增三级滚动与总计/最大值保留|
|HistoryIncludesPartialIntervals|ZStatSamplerHistory avg/max getters|新增未满分层窗口合并|
|StwDepthCounterClassifies|既有 GCLOG 观察合同（不作为 ZGC registry 能力）|保留|
|ZeroDurationSampleStillCounts|ZStatSample/collect_and_reset|保留，迁移到 sampler API|

机械集合差 `local:{E}/test-set-diff.txt`。本轮不为了判据变绿改测试或添加豁免；旧用例替换的原因是被测旧机制确已删除。

测试构建尝试：`cj_gc_unit` 在提交 8084195d2aaf372d5ff8b0e9b620a3ef8fe6776d 编译目标 rc=1、wall=47s；原始日志 `local:{E}/remote/build-test-sources.log`、`test-sources.rc`、`test-sources.head`。失败涉及既有 fixture 的旧 forwarding API、ResolveStoreValue 签名和 worker 参数；失败文件与基线 blob 身份对照见 `local:{E}/test-failure-content-identity.txt`。本包 test_zstat.cpp 已生成对象；未执行任何测试，不宣称测试通过。最终测试 TU 构建结果另见 `test-zstat-object.rc`。未生成测试 ELF，六栏卡的测试 ELF 摘要不适用。

## 产品接线证明

|对象|产品接线|行为验证状态|
|---|---|---|
|phase sampler|静态 phase → 产品 Timer → RegisterEnd → Sample|源码图完整；本轮不跑行为臂|
|allocation counter|RegionManager 成功分配 → ZStatMutatorAllocRate → counter → tick/history|源码图完整；本轮不跑行为臂|
|generation/collection|RunCollection / ExecuteDriverRequest → 相应静态 phase|源码图完整；本轮不跑行为臂|
|history/输出|统计线程 → SampleAndCollect → history Windows → Print|两构型符号在场；运行时行为未验证|

## 承重面清单

机械导出为 `local:{E}/producer-chain.txt`，包含每个 phase 调用、allocator 出口、driver 两条身份出口、history 与线程生命周期。全部未执行切刀/三臂：依据本轮 alignment_mode 明确例外。不存在红臂行为验收结论。

## 授权与候选边界

- 首次 advisor 允许 GcStats/MutatorAllocRate 本体、Timer 及各统计调用点，`/root/cj_build/ops/advisor/outbox/{lane}-20260913T040604Z.md:2`。
- 第二次允许仅删除两处父级构建旧开关登记，确认 Timer 实际在 LogFile.h，`/root/cj_build/ops/advisor/outbox/{lane}-20260913T041609Z.md:2`。
- 第三次 oldLive 裁定见上文，保留语义并分派 A07 后续。
- 基线 rev-parse rc=0，完整身份及 commit payload sha256 / Git object SHA1 独立复算见 `local:{E}/identity.txt`。只推 cjcjdev 的本候选分支。

|候选提交|机制|
|---|---|
'''
for line in subprocess.check_output(['git','log','--reverse','--format=%h %s',base+'..HEAD']).decode().splitlines():
 sha,title=line.split(' ',1);text+=f'|`{sha}`|{title}|\n'
text+=f'''
首笔同时迁移 registry/history 与统计生产消费本体，判词只对本包整体成立，不把提交数当独立变量数。

## FALSIFIED

1. 指定 zStat.inline.hpp 在参考树不存在；采样实现实际位于 zStat.cpp:883 起。已获 advisor 确认，按实际文件对齐。
2. 原排他文件集不足以迁移 GcStats/MutatorAllocRate 和真实 Timer；通过两次 advisor 获准精确扩展，未自行扩大其它机制。
3. 实现中初次选用 std::align_val_t 不符合产品 C++14，两个实际编译失败后按 zUtils.inline.hpp:37-49 修正，未改编译标准。
4. 初次运输包漏根 tree、首次构建配方工作目录错误，均已修正后再记录产品编译结果；不把这些失败当作产品行为证据。
5. oldLive 真实 mark-end 汇总不是现有基线语义，依明确裁定留作 A07 后续；移除中间态 candidate-reclaimed 推算。

## 后续项

SYM-NEXT: stage=Triage new-issue: cangjie-runtime | ZStatHeap old live 改为 mark-end livemap 汇总（前置 A07） | Triage | ③ perf
SYM-NEXT: stage=Triage new-issue: cangjie-runtime | gc_unit 测试目标仍引用冻结基线已移除的产品接口，无法完成构建 | Triage | infra

DELIVERY_CHECK_PLACEHOLDER
PR_PLACEHOLDER
'''
Path('evidence/report-draft.md').write_text(text)
print('report generated',len(text))
