# #646 开发集成坐标（WIP，不能作为正式验收）
待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。

- candidate: cangjie-runtime|sym/646-implement-r5686136394|ca189d90ec7cc158d2b7e4891a0f2a52ca12a920
- kkk2 default SO: /root/sym_cangjie_runtime_646_implement_r5686136394-bridge/default/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so
- runtime sha256: a1d83eb6b80ca3024859f623bf5e77e0f779e8918294d394b38458d626999271
- 同目录 libboundscheck.so sha256: f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883
- testable SO: /root/sym_cangjie_runtime_646_implement_r5686136394-bridge/testable/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so，sha256=94b8d14e71e9a9a323a97615c18801b364c8cc33488c268238de642ba92e6436。
- 四managed桥 CJ_MCC_PackageInit{Begin,Complete,Fail,Abort} 已用现有各平台 CalleeSavedRegistersStub；裸MCC四实现保持既定签名和值。Begin不可标gc-leaf，Complete/Fail noexcept，Abort noreturn+noexcept。phase/cache-only/reset策略不变。
- 当前定向尺: kkk2:/root/sym_cangjie_runtime_646_implement_r5686136394-bridge/focused2/run.log，14项13过1失败；唯一失败 NativeDlcloseAllowsDependency。真实dlclose/fini待owner时，owner的依赖Begin需平台loader查询，当前构造未完成目标断言。线程栈 native-dlclose-backtrace.log，gdb_rc=0/run_rc=1。映射修正将生成新目录/SO，不覆盖此产物。
- 可供#48提前编译/链接开发；不得把此WIP SO标为整体通过/正式可合并。最终真实compiler生成物资格仍待#48。

## 更新：1d35dc8b1f84761323f1c7b5ae54aee5bb9ea471（仍WIP）
- kkk2 default: /root/sym_cangjie_runtime_646_implement_r5686136394-final/default/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so；sha256=7f5865ecc36786e3e690396fc10f63c03866685bed7d315667894b7f72687336
- kkk2 testable: /root/sym_cangjie_runtime_646_implement_r5686136394-final/testable/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so；sha256=3925957608aa838f46aad37eb0692279edeaf9e55ed53d449e8c25dd91c31617
- 两目录boundscheck均f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883。两构型rc0各15s，default定向16/16、testable定向18/18 rc0；真实NativeDlcloseAllowsDependency现已通过。红臂与最终全套仍进行中，不能据此标正式放行。
- 发布暂停接缝头文件/API与调用顺序：coordination/PACKAGE_INIT_TEST_HOOKS.md。仅宏开有三个MRT_PackageInit*CompletePause导出，原四CJ_MCC桥两构型都有。#48原生harness可用该header（define MRT_TESTABLE_INTERNALS）并链接宏开SO；其生成代码仍调用真实CJ_MCC四桥。

## 更新：d4864ab93355dbaaee4a3a274f54c93e960761f7（WIP，已合P02）
基于真实main b38fcfdef926f6ae5dff2e8d0ffa8f4d6356bc18接回。已增加同header的只读MRT_PackageInitHasWaitingCaller(package,unit,phase)，只扫描现有waitingOn。
- default SO: kkk2:/root/sym_cangjie_runtime_646_implement_r5686136394-delivery/default/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so；sha256=5a4fade802a1f3df491e07cd43965b582ce1f5e5a6c06e59af9e18524a373259
- testable SO: kkk2:/root/sym_cangjie_runtime_646_implement_r5686136394-delivery/testable/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so；sha256=2d68aab6e7d26d23b391a0230290db7dbb968b1ffb02b5bfaaca91614f58deac
- 两目录boundscheck sha256仍f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883。两构型configure/build rc0，wall15s/14s。更新后的只读观察断言尚待小型fixture运行，不把先前18/18直接算作新观察的证明。
kkk2容量暂停大构建中；主控恢复容量后继续同头单构型故障臂，保留旧臂与所有原产物。

## 真实PinArray消费者修正：0d16b0448f22c18b1b2f0b443d7dda28cfd3b97b
LibInit native saferegion只覆盖admission/lookup；DoInitImage实际进入ScopedObjectAccess后执行托管body，返回恢复caller状态。PinArray断言未动。
- default SO: kkk2:/root/sym_cangjie_runtime_646_implement_r5686136394-managed-entry/default/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so；sha256=e59d82887208a83a0c835d491141d19151bcf71443778688cdee24d4c57c3497
- testable SO: kkk2:/root/sym_cangjie_runtime_646_implement_r5686136394-managed-entry/testable/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so；sha256=0bc6980f90abceea33b1c9c2bdfa6495d5bf586a0c22baaa01dbb04fb9fc8a5a
- boundscheck两目录均f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883。两构型rc0；该版本focused default18/18、testable20/20 rc0，libinit-body-has-managed-access实际pass1；standalone default523/523rc0，testable727/729rc1仍为两项已有同名失败。
- #48请用同真实DLL/ELF复验未匹配control的PinArray、再复验Complete暂停/真实waitingOn/期间GC，不以本原生fixture代替。
当前候选2a3567ee3仅新增两scheduler worker测试；git diff --exit-code 0d16b0448..HEAD -- runtime/src runtime/CMakeLists.txt runtime/config.cmake rc0，产品源码逐字相同，可复用以上两SO。

## 交付候选：P06接回后的产品 b73d2491c69e5f0dd5c61f7216ea64d3ceb4623a
待独立Review；本条是最新产品坐标，上文保留为过程证据。
- main=91f3dcc232201d4ad98ec3af6f10165edb950416，已merge。
- default SO: kkk2:/root/sym_cangjie_runtime_646_implement_r5686136394-p06/default/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so；sha256=9fa62977f0e96c63d54e439351b57867a0fba4a8a5548a2ea7849a4a4f128a10。
- testable SO: kkk2:/root/sym_cangjie_runtime_646_implement_r5686136394-p06/testable/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so；sha256=03460b1f482f7a7fddd103246ffad2a56d128f49e46451d7f60d0aa1853e9810。
- 两目录boundscheck SHA=f18a1393f84d56a455c71c0c28bf1752c1778af06c0648c1b7cffa91c585c883；不可只拿runtime SO。
- 两构型configure/build rc0、wall各27s、-j192；本包default19/19、testable21/21；全套default554/554、filler同default ELF554/554；testable758/760，失败NativeRootCurrent.MajorSeed、ColourCensus.PlainWriteFunnelFailsClosed，未改断言。
- 原0d16源码的14组精确切刀和恢复证据保留；相关Loader/Mutator/bridge函数在P06合并后未变。最新组合重新构建/全套记录另列，不把不同SO混成三臂。
- 完整交付报告：/root/cj_build/reports/REPORT-sym_cangjie_runtime_646_implement_r5686136394.md。
- #48后续真实compiler/library/reset整体资格仍单独报告。本原生fixture不替代compiler消费资格；公共多卸载者场景按独立探索/归并记录，未声称覆盖。
