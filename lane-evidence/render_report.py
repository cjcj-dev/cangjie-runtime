from pathlib import Path
import json
lane='sym_cangjie_runtime_581_implement_r5668277053'
e=Path('/root/cj_build/reports/EVIDENCE-'+lane+'/final3')
r='/root/'+lane+'-final3'
w='/root/cj_build/cangjie_runtime_wt/'+lane
head='4ae08d2033af7d35d0466264b5b601313527dc48'
results=json.loads((e/'results.json').read_text())
def sha(arm):
 for line in (e/(arm+'-run-so.sha256')).read_text().splitlines():
  if line.endswith('/libcangjie-runtime.so'):return line.split()[0]
rows=[]
for arm in ['unit-default','unit-filler','run-producer','run-promotion','run-phase','run-consumer','run-entry','run-restored']:
 x=results[arm];t=x['totals'][0];rows.append(f"|{arm}|{x['rc']}|{t[0]}|{t[1]}|{t[2]}|{e}/{arm}.log|")
report=f'''PROGRESS=WIP · verdict=真实晋升与Acquire入口的目标断言及五刀验证已闭合，整理交付 ｜尺=run_standalone/run_parallel_tests 目标=10 N=每臂1次、布局每相位3例 · LANE={lane}
DELIVERY_REF=cangjie-runtime|sym/581-implement-r5667741628|{head}
SIDE_EFFECT: 原候选分支已推送、复用PR#586；合并#579；故障仅在kkk2本棒独立副本；新增入口前置读取问题已登记。
ROLE=implement
EVIDENCE=local:{e},kkk2:{r}
LANE={lane}
ROLE=implement
PROGRESS=WIP

## 坐标及范围

指定仓回读 `git -C /root/cj_build/cangjie_runtime rev-parse cjcjdev/main` 得e26fcb34329464aefefb08873795faae80f51366，rc=0。任务给定333d3216762495d49d34594390c3afa05378010f是上一轮候选HEAD。advisor于01:59确认返工base指候选，继续原分支；三份原文：/root/cj_build/ops/advisor/outbox/{lane}-20260914T175403Z.md、同前缀175558Z.md、175637Z.md。

本轮新增六个测试并保留原四项；产品修法沿用上一轮：Pin按carrier源代选相位，PREFORWARD/FORWARD经relocate_or_remap_object解析后增加计数并返回。交付前fetch及merge主线fca1c8ce77f139b14cef9117c1246fe01b0aa084，rc=0；#579与本包文件尾冲突保留双方测试，合并提交23aeb468723fb4334fc30909a0ed1d1aef5650b9。最后一次同步Already up to date，推送HEAD={head}到原分支，rc=0。未操作main分支。相对当前主线净差分为WCollector.h与clear_entries_product_unit.cpp。

#579已合入，不再是阻塞依赖；本包未另改其页代真值源。无CHECK放宽、新豁免或测试开关。未自审，交独立Review。

## ZGC对应与producer→consumer顺序

改码前顺序表原文：local:{e}/producer-consumer.md；下表以最终HEAD补锚。

|顺序|产品锚|ZGC及不变量|
|---|---|---|
|晋升生产|runtime/src/Heap/z/zRelocate.cpp:2065 → zPage.inline.hpp:1424|zRelocate.cpp:862–896：先建立目标页代，再发表位移映射；carrier保留源代|
|复制及发表|zRelocate.cpp:2082 CopyObject、:2087 InsertMapping|zRelocate.cpp:652–731：结果对象/映射一致|
|真实重用|zRelocate.cpp:2097 ZeroAndFill、:2107 RehomeCompactedInPlaceRegion|zRelocate.cpp:1001–1047：转发表跨页面重用保留|
|运行时入口|arch/x86_64_linux/CalleeSavedStub.S:94 → CompilerCalls.cpp:945 Acquire → :969 PinArray → :937 虚调用Pin|调用层结果须流向payload；入口并非测试内拼装Pin函数|
|选代及解析|WCollector.h:450–458|zGeneration.inline.hpp:131–140、zRelocate.cpp:382–415：按所属代表处理映射|
|计数与返回|WCollector.h:464–465 → CompilerCalls.cpp:991；Release :1024|目标页Inc与rawPtr所属页Dec配对|

ZGC根固定为/root/cj_build/reference/jdk/src/hotspot/share/gc/z/。上述为职责与顺序对照，不宣称JDK具有同名Acquire接口。

## 产品接线证明

最终测试源：{w}/runtime/tests/gc_unit/clear_entries_product_unit.cpp:3010。使用Heap持有的collector，dladdr读取其虚表归属并进入断言；产品CompactRegion、CJ_MCC_AcquireRawData、CJ_MCC_ReleaseRawData均由测试ELF导入。`nm --defined-only`完整清单、版本化导入及main阳性对照在local:{e}/symbols.txt；产品Pin定义与实际SO路径见每条RAW_ACQUIRE_ASSERT。Pin/Compact/Acquire/Release没有测试内定义，阳性对照是同一ELF的main及产品SO对应定义，不能用nm -D代替这份证据。

|测试（RawPinProduct.前缀）|真实产品函数|断线后目标结果|闭环自评|
|---|---|---|---|
|AcquireAfterCompactPreforward / AcquireAfterCompactForward|CompactRegion:2065→Acquire:945→PinArray:937→Pin:458|promotion刀页代断言3109；phase及entry刀payload断言3102；consumer刀countPinned断言3103|✅ 真实生产状态、产品虚表、结果断言和因果臂均有产物|
|AcquireSourceIdleOtherForward|同上，源代IDLE/另一代FORWARD|phase刀使from payload错误变为to，3102失败|✅ 反向相位有结果差分|
|AcquireGhostWithoutCarrier / AcquireUnmovableGhost / AcquireWithoutForwarding|Acquire→产品Pin绕过分支→Release|consumer刀3103失败；正常返回原地址且计数配对|✅ 对本分支地址/计数，不外推晋升周期|
|原有四项|产品Pin→TryMutatorRelocate/已有映射|producer/phase/consumer在2969或2971失败|✅ 原消费证据保留；人工输入不冒充真实晋升|

正式原始路径：local:{e}/unit-default.log、run-producer.log、run-promotion.log、run-phase.log、run-consumer.log、run-entry.log、run-restored.log；每项隔离日志在同目录相应run-*/test-logs下。被测SO和ELF持续保留于kkk2:{r}及其各臂同前缀目录。

## 确定布局与界限

每个正向相位及反向相位各运行length=16/32/48三例（N=3不同确定尺寸，各独立重置页面；不是重跑取绿）。对象含16字节头，死前缀分别32/48/64字节，活数组放在等长偏移；PrepareForwardable仅标活第二对象，产品CompactRegion将其移到页首并晋升，随后产品filler头恰落在原from。日志逐例记录from≠to、receipt=to、filler=1、promoted=1、payload和0→1→0计数；完整payload按0x5a字节比较，非仅比较地址。复制和晋升均由产品完成，没有手工恢复旧头或设置晋升结果。

**同一调用闭环仅对from恰为产品filler数组头的布局成立**，不是任意“落在filler内部”都可读。这里只构造GC输入与相位，未声称跑完整GC调度周期或真实CJThread抢占调度。顺序验证不作为性能证据。三个尺寸在一个用例内顺序执行，因为复用同一进程全局页表、Heap和fixture；各用例及各臂并行。

## 承重面清单

机械检索命令：`git grep -n -E 'PinRawPointerObject|PinArray|MCC_AcquireRawData|MCC_ReleaseRawData|CompactRegion|PromoteYoungRegion' HEAD -- runtime/src`，输出每个命中整行见local:{e}/consumer-inventory.txt。其中声明/注释与实际调用分别处理如下；本函数无full/range或worker数分支轴。

|消费者/分支|产品锚|臂/rc与边界|
|---|---|---|
|OLD未复制：PREFORWARD、FORWARD|WCollector.h:458→:412→TryMutatorRelocate|producer rc=1，原两项地址断言|
|YOUNG carrier、已晋升OLD页：PREFORWARD、FORWARD|WCollector.h:453、:458|phase rc=1，真实Acquire两项及原人工两项|
|源代IDLE、另一代FORWARD|WCollector.h:454条件不成立|phase rc=1，真实反向项；该项验证非重定位分支，不逐一枚举同一布尔分支的所有phase枚举值|
|无ghost|WCollector.h:445|AcquireWithoutForwarding，consumer rc=1|
|ghost无carrier|WCollector.h:450–451|AcquireGhostWithoutCarrier；产品ResetRelocationSet后保持ghost，consumer rc=1|
|unmovable ghost|WCollector.h:446|AcquireUnmovableGhost，经产品Exempt重挂，consumer rc=1|
|运行时虚调用及返回payload|CompilerCalls.cpp:937、:969、:991|entry rc=1，仅真实正向两项|
|release按结果页减计数|CompilerCalls.cpp:1024|全部Acquire绿/恢复臂检查配对；consumer刀验证其前置pin确有发生|
|obj=null|WCollector.h:444|Acquire在非heap/null分支直接返回，未调用Pin；不属于本题ghost解析臂，底层AddRawPointerObject并非null契约|
|void AddRawPointerObject→Pin|WCollector.h:424|既有丢弃返回值的另一路，不是本轮修复目标；输入#583已有记录，不把上述Acquire证据外推到其返回值消费|
|CompactRegion其它调用点、PromoteYoungRegion其它调用点、Collector基类虚声明|见机械完整清单|本包消费输入生产面以实际Compact为界，不修改其它迁移/晋升调度；不声称所有GC生产入口已测|

## 断线与恢复

每刀只改一处产品源码，补丁见local:{e}/cut-*.diff。各刀均在基线333d321…已有行；entry_cut_check.py对最终HEAD校验rc=0，JSON={e}/entry_cut_check.json，至少一刀落于真实入口TryMutatorRelocate。该检查仅核切点形式，不代替结果验证。

|刀|基线产品切点|目标断言及范围|
|---|---|---|
|producer|zRelocate.cpp:1372，返回toVersion改回obj|原两项地址2969；另一个既有同机制MutatorRuntimeEntryReachesCopyAdmission也失败|
|promotion|zRelocate.cpp:2065，跳过PromoteYoungRegion调用|5项真实Compact输入在3109的promoted断言失败；地址/计数先通过，不是更早失败遮住晋升断言|
|phase|WCollector.h:453，恢复按当前页代选相位|真实正向两项和反向一项在3102失败，另两个人工晋升项2969失败|
|consumer|WCollector.h:464，去掉AddRawPointerObject|10项计数目标失败；原四项2971，新增六项3103。新夹具仅在观察到获得pin时release，不制造计数，不绕过目标countPinned==before+1断言|
|entry|CompilerCalls.cpp:938，PinArray返回原array|仅真实正向两项在3102地址目标失败，证明调用方消费Pin结果|

所有正式臂N=1次同名单运行；各条RAW_ACQUIRE_ASSERT输出产品状态与返回值后进入断言。目标通过证据为绿/恢复相应用例PASS及逐例输出；目标失败证据为红臂相应EXPECT位置。没有编译/加载失败充当红。

|臂|rc|总项|通过|失败|原始日志|
|---|---|---|---|---|---|
{chr(10).join(rows)}

逐项失败名单与所有RAW_ACQUIRE_ASSERT/EXPECT摘录：local:{e}/results.json。其余失败差集没有扩散到本机制外；阳性对照就是上述五刀，不以恒绿推断装置有效。

## 测试增删

集合差及坐标：local:{e}/test-delta.json。相对上一候选新增本包六项AcquireAfterCompactPreforward、AcquireAfterCompactForward、AcquireSourceIdleOtherForward、AcquireWithoutForwarding、AcquireGhostWithoutCarrier、AcquireUnmovableGhost，另两项PageGeneration579来自合并。相对主线本包10项，原测试名集合保持；新增集合为零删除核对的阳性对照。无删除/改名/改旧期望。分类(iii)产品语义修复的真实入口回归，未以豁免处理既有失败。

## 双构型证明及产物身份

统一入口/root/cj_build/ops/bin/kkk2_build_two.sh，default变量MRT_TESTABLE_INTERNALS=OFF、testable=ON，两者最终构建rc=0（57/56秒）。各刀/恢复复用该脚本生成的同一remote_build.sh配方，仅隔离目录及受控源码差；default/testable成对并行，五刀+恢复合计12个构建臂，另OHOS一臂并行。实际-j192，测试编译GC_UNIT_BUILD_JOBS=192，runner jobs=192；核域由cjops windows领取32–63，同组因果臂共用该核域。无步骤wall>10分钟。wall及构建日志在各build-*/目录和*.wall。

|构型|构建rc|产品SO sha256|
|---|---|---|
|default，OFF|0|{sha('green')}|
|testable，ON|0|d67fec2d91d98dc49fbfe1eabadb5a5e2bc85d3360a6014d2f373045488b84d5|
|OHOS-host，MRT_GC_UNIT_OHOS_HOST=ON、OFF|0|见local:{e}/build-ohos/default-so.sha256|

UNIT_DEFAULT_RC={results['unit-default']['rc']} kkk2:{r}/unit-default.log
UNIT_FILLER_RC={results['unit-filler']['rc']} kkk2:{r}/unit-filler.log
UNIT_OHOS_RC={results['unit-ohos']['rc']} kkk2:{r}/unit-ohos.log

filler使用现行gate口径CJRT_HEAP_FILLER=0；三项OHOS过滤用例各rc=0，原始receipt={e}/unit-ohos/ohos_host.receipt。OHOS源目录fetch精确候选HEAD后执行runner，ohos-git.rc=0；源码tar本身不含.git，未伪造产品stamp。

正式因果臂共用同一两枚ELF，sha256全文见local:{e}/elf.sha256；产品各臂链接后立即由统一脚本存*-so.sha256，运行前再次核对保留副本。运行后仅读取原产物，未事后重建替换证据。

|产品default臂|runtime SO sha256|
|---|---|
{chr(10).join('|'+arm+'|'+sha(arm)+'|' for arm in ['green','producer','promotion','phase','consumer','entry','restored'])}

绿=恢复SO逐字节相同；boundscheck与trace各臂相同，只有runtime SO随刀变化。血缘stamp全文在green-stamp.txt、各*-stamp.txt，源码tar哈希在source-archive.sha256。full nm双向对照包含@CANGJIE版本化导入，不以substring四个Acquire命中误报为四个函数（另两个是静态局部变量）。产品身份与因果验证详见symbols.txt。

核域与两端uptime原文：local:{e}/windows.txt、unit-uptime-before.txt、unit-uptime-after.txt、tests-uptime-before.txt、tests-uptime-after.txt；这些为条件记录，不下性能结论。

## 主线内容核

local:{e}/main-content.txt逐条保留git grep -c的命令、完整输出和rc。#579相关generation_id、CloneForPromotion、RetainPageOwner及PageGeneration579在head计数不低于main；#579改的四个产品文件与main逐字节相同。测试集合差main_missing为空，新增10项为正向对照。此为内容保留检查，不是实现者自审。

## CLAIM

CLAIM: 真实Compact晋升产生位移输入，经产品Acquire虚调用返回正确payload并完成同页pin/release配对。
  METHOD: test
  EVIDENCE: local:{e}/unit-default.log，RawPinProduct.AcquireAfterCompactPreforward/Forward；{w}/runtime/tests/gc_unit/clear_entries_product_unit.cpp:3102；local:{e}/run-entry.log与run-restored.log。
  N: 每臂1次，每个正向相位3个确定尺寸

CLAIM: carrier源代相位选择对真实正向/反向输入均因果敏感。
  METHOD: test
  EVIDENCE: local:{e}/run-phase.log；目标payload断言{w}/runtime/tests/gc_unit/clear_entries_product_unit.cpp:3102；local:{e}/run-restored.log。
  N: 每臂1次

CLAIM: 产品晋升调用与目标页pin计数分别可在目标不变量处精确转红。
  METHOD: test
  EVIDENCE: local:{e}/run-promotion.log、run-consumer.log、run-restored.log；{w}/runtime/tests/gc_unit/clear_entries_product_unit.cpp:3109、3103；cut-promotion.diff、cut-consumer.diff。
  N: 每臂1次

CLAIM: 最终绿/恢复产品相同，而各刀runtime身份不同；其它SO与测试ELF固定。
  METHOD: control-arm
  EVIDENCE: local:{e}/green-run-so.sha256、restored-run-so.sha256、producer-run-so.sha256、promotion-run-so.sha256、phase-run-so.sha256、consumer-run-so.sha256、entry-run-so.sha256、elf.sha256。
  N: 绿/恢复各1次，5个受控刀各1次

## FALSIFIED

1. 任务冻结ref与sha对应关系由实测否定，advisor确认冻结sha实际为上一轮候选，见坐标节。
2. 自我更正：最初将“旧头清除”推断为所有位移from都不能经过Acquire过强；HeapFiller.cpp:72–92的产品filler头提供确定可读布局。三尺寸真实日志已验证；只对精确filler头布局下结论。
3. 开发夹具初次包含CompilerCalls.h触发实现专用alias编译错误，initial rc=123；改公开C入口声明。随后重复回收live-map触发pageIt守卫，debug rc=1；按LiveInfoArena.h:72整页回收修正。这些不是产品红臂。
4. final2计数刀下六个新用例先触发Release守卫，没有到目标断言，不计入正式证据；旧日志保留在kkk2:/root/{lane}-final2/run-consumer.log。最终final3仅release实际获得的pin，仍严格断言必须+1，所有10项均在目标计数断言失败。其它最终状态只认本报告final3产物。

## 后续项

advisor明确分拆Acquire/release前置数组头读取问题：CompilerCalls.cpp:960/963在:969 Pin前读头；MArray.inline.h:38–52经TypeInfo读取component；HeapFiller.cpp:72–92只在filler起点生成头、其内部清零。因此一般位移from若不是有效filler头（例如dead prefix64/live数组32，from位于filler内部），前置读取可能在Pin前失败。本轮为源码证据，未将该一般布局记为运行验收；已通过MCP new_issues登记Triage，不扩改本包。

SYM-PR: cangjie-runtime#586
SYM-NEXT: stage=Triage new-issue: cangjie-runtime | MCC_AcquireRawData在Pin解析前读取已位移from数组头，非filler头布局不能到达解析 | Triage | ① correctness
'''
Path('/root/cj_build/reports/REPORT-'+lane+'.md').write_text(report)
