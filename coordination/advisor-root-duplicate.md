LANE=sym_cangjie_runtime_727_implement_r5745195443
第五处修复 a283c386f：GcPhaseEnum 在 InstallMasks 前捕获 ThreadGCData.loadGoodMask，PushHeapRoot 两分支传该色。双构型rc0；managed各目标三发rc0（kkk2:/root/diff_a283c386f09a/managed-runs/kkk2_managed.json）。
新增定点用例构造产品CompactRegion原地搬迁后经真实GcPhaseEnum消费：普通根已到地址目标断言且地址正确（收尾误Withdraw未发布root，已准备修夹具）；不可见根尚未到断言，在第二次remap失败。
源码：runtime/src/Heap/z/zStackWatermark.cpp:78 VisitRawObjects(invisibleRootVisitor)，后:79-84 又读gcData.invisibleRoot且process_invisible；Mutator.h:64 gcData.Attach明确两者为同一rawObject槽。第五处使第一次消费也按旧色remap，第二次再以旧色处理已更新地址。日志 kkk2:/root/sym_cangjie_runtime_727_implement_r5745195443/unit-new.log:4492-4496，target=0x40000200000、缺forwarding entry。
ZGC zStackWatermark.cpp:164-174 process_head仅调用一次ZUncoloredRoot::process_invisible；我方该双消费属既有独立分歧。请求裁定：这是第五处消费接线必须同批删重复，还是第六处须本轮TRIAGED、另派？暂不改该产品点、不弱化断言；继续普通根精确断线证据。另请确认eager完整扫描下按值捕获线程旧色符合本轮范围（无return statepoint，非分段帧恢复）。
