待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`
LANE=sym_cangjie_runtime_1222_implement_r5857705060
任务正文允许 sanitizer 区间长度按基础设施差异处理；常备裁决6只限四类，不含 sanitizer。实读 CompilerCalls.cpp:1113 传 plain->GetContentSize；AsanInterface.cpp:195 消费长度 poison；GwpAsanInterface.cpp:181 消费长度 CheckCanary；HwasanInterface.cpp:184 又读 array->GetLength。ZGC jni.cpp:2889-2893 resolve 后 unpin，zCollectedHeap.cpp:279-281 仅 exit。
拟最小方案：删除 Release 的 IsPrimitiveArray 检查；sanitizer acquire 时在已有 counter/canary 记录区间长度，release 仅凭 rawPtr 查记录，不读 array；删除 HWASAN release 的对象头校验（ZGC 无对应）。是否授权 sanitizer 配对元数据为本条明确基础设施例外？否则是否要求删除整套 rawdata sanitizer acquire/release 装置？本棒先继续真实入口测试与数据流枚举，不改争议产品部分。
