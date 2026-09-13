| 原文件（行锚基于冻结 5e04db89） | 本轮处置 / 承载 | ZGC 理由 |
|---|---|---|
| `runtime/src/Heap/Verify/AllocPhaseDiag.h:1` | 删除 | 分配相位戳和近端ring；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/CMakeLists.txt:1` | 仅登记 `../z/zVerify.cpp` | 仅登记最终保留与迁入组件；删文件不留源清单 |
| `runtime/src/Heap/Verify/CsetEmptyWho.cpp:1` | 删除 | 空候选页反向根census；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/CsetEmptyWho.h:1` | 删除 | 空候选页反向根census；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/DiagGate.cpp:1` | 删除 | 环境token/legacy别名目录；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/DiagGate.h:1` | 删除 | 环境token/legacy别名目录；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/FillerZeroDiag.cpp:1` | 删除 | zero-fill站点观测；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/FillerZeroDiag.h:1` | 删除 | zero-fill站点观测；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/FromPageDetachCheck.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | reuse前detach义务保留到页生命周期；平行census/PermitScope/quarantine不是ZVerify，D03先收口 |
| `runtime/src/Heap/Verify/FromPageDetachCheck.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | reuse前detach义务保留到页生命周期；平行census/PermitScope/quarantine不是ZVerify，D03先收口 |
| `runtime/src/Heap/Verify/GarbRegionDiag.cpp:1` | 删除 | 垃圾region引用census；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/GarbRegionDiag.h:1` | 删除 | 垃圾region引用census；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/HealCoverage.cpp:1` | 删除 | 颜色复用前全堆census；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/HealCoverage.h:1` | 删除 | 颜色复用前全堆census；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/HealPairDiag.cpp:1` | 删除 | 零写入与信号现场ring关联；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/HealPairDiag.h:1` | 删除 | 零写入与信号现场ring关联；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/HoleWhoDiag.cpp:1` | 删除 | 对象步进中断ring；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/HoleWhoDiag.h:1` | 删除 | 对象步进中断ring；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/InteriorEdgeClass.h:1` | 删除 | 私有interior分类器；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/M0Correlation.cpp:1` | 删除 | 外部key对象stamp因果账；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/M0Correlation.h:1` | 删除 | 外部key对象stamp因果账；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/M0ExitDiagnostics.cpp:1` | 删除 | 解析退出采样分类；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/M0ExitDiagnostics.h:1` | 删除 | 解析退出采样分类；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/MarkCompleteVerify.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 强根/old引用语义有对应；RunAtMarkEnd及CheckEdge/CheckRoot移到ZVerify图入口，WatchHolder/TraceWatch采样删除 |
| `runtime/src/Heap/Verify/MarkCompleteVerify.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 强根/old引用语义有对应；RunAtMarkEnd及CheckEdge/CheckRoot移到ZVerify图入口，WatchHolder/TraceWatch采样删除 |
| `runtime/src/Heap/Verify/MinorGCALot.cpp:1` | 删除 | 通用HotSpot ScavengeALot触发，非ZGC对应机制；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/MinorGCALot.h:1` | 删除 | 通用HotSpot ScavengeALot触发，非ZGC对应机制；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/NwDropAudit.h:1` | 删除 | 负载专用remset丢弃分类；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/StackExposureOracle.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 旧栈exposure双跑oracle；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/StackExposureOracle.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 旧栈exposure双跑oracle；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/StackFrameOracle.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 旧栈frame双跑oracle；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/StackFrameOracle.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 旧栈frame双跑oracle；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/StackWatermarkOracle.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | watermark STW exercise oracle（不是watermark产品本体）；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/StackWatermarkOracle.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | watermark STW exercise oracle（不是watermark产品本体）；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/Stw2CurrentAudit.cpp:1` | 删除 | STW2 current面全量census；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/Stw2CurrentAudit.h:1` | 删除 | STW2 current面全量census；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/SurvNodeDiag.cpp:1` | 删除 | 对象paint/follow/store因果ring；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/SurvNodeDiag.h:1` | 删除 | 对象paint/follow/store因果ring；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/TraceClear.cpp:1` | 删除 | payload清零地址ring；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/TraceClear.h:1` | 删除 | payload清零地址ring；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/VerifyHeap.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 对象/字段有效性有对应，物理inventory不能充作reachable图；Tip页探查/采样分类删 |
| `runtime/src/Heap/Verify/VerifyHeap.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 对象/字段有效性有对应，物理inventory不能充作reachable图；Tip页探查/采样分类删 |
| `runtime/src/Heap/Verify/VerifyMarkingStacks.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | stack empty本体对应ZMark，不能删除；移除自创矩阵receipt/日志/diag gate |
| `runtime/src/Heap/Verify/VerifyMarkingStacks.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | stack empty本体对应ZMark，不能删除；移除自创矩阵receipt/日志/diag gate |
| `runtime/src/Heap/Verify/VerifyOption.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | G1/shared verifyOption式自选mark源没有ZVerify消费者；错误私有图由D06a替换 |
| `runtime/src/Heap/Verify/VerifyOption.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | G1/shared verifyOption式自选mark源没有ZVerify消费者；错误私有图由D06a替换 |
| `runtime/src/Heap/Verify/VerifyPhase.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 自创五面名/环境token不是语义本体，换真实ZVerify phase调用 |
| `runtime/src/Heap/Verify/VerifyPhase.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 自创五面名/环境token不是语义本体，换真实ZVerify phase调用 |
| `runtime/src/Heap/Verify/VerifyRegions.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | region-list/candidate账不是ZVerify的Oops地址检查，D06a替换Oops真实语义 |
| `runtime/src/Heap/Verify/VerifyRegions.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | region-list/candidate账不是ZVerify的Oops地址检查，D06a替换Oops真实语义 |
| `runtime/src/Heap/Verify/VerifyRememberedSet.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 保留before/after relocation与after_scan的真正remset不变量，替换全堆snapshot比较 |
| `runtime/src/Heap/Verify/VerifyRememberedSet.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 保留before/after relocation与after_scan的真正remset不变量，替换全堆snapshot比较 |
| `runtime/src/Heap/Verify/VerifyRoots.cpp:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 保留colored/plain根有效性；替换独立oracle/原始寄存器采样与自创scene统计 |
| `runtime/src/Heap/Verify/VerifyRoots.h:1`（冻结时不存在） | 冻结时已迁走/删除；本轮不重复归功 | 保留colored/plain根有效性；替换独立oracle/原始寄存器采样与自创scene统计 |
| `runtime/src/Heap/Verify/Zap.cpp:1` | 删除 | 自创填充值/分配及回收模式；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/Zap.h:1` | 删除 | 自创填充值/分配及回收模式；ZVerify完整入口族不生产/消费该私有记录 |
| `runtime/src/Heap/Verify/ZgcInvariants.cpp:1` | 删除；自创 tuple/地址相等计数删除，现有屏障同一 good 地址数据流保留 | 保留可映射到assert_is_valid/self_heal的本体断言；global tuple/stale census/统计删除 |
| `runtime/src/Heap/Verify/ZgcInvariants.h:1` | 删除；自创 tuple/地址相等计数删除，现有屏障同一 good 地址数据流保留 | 保留可映射到assert_is_valid/self_heal的本体断言；global tuple/stale census/统计删除 |
| `runtime/src/Heap/Verify/ZgcSelfHealDiag.cpp:1` | 删除；本体已在 `zBarrier.cpp:36` / `RefField.h:322` | 单调性/precondition对应self_heal本体，内迁；其余计数/重试census/开关删除 |
| `runtime/src/Heap/Verify/ZgcSelfHealDiag.h:1` | 删除；本体已在 `zBarrier.cpp:36` / `RefField.h:322` | 单调性/precondition对应self_heal本体，内迁；其余计数/重试census/开关删除 |
| `runtime/src/UnwindStack/StackExposureHook.cpp:1` | 删除 | 自创 hook + 观测计数；唯一产品调用只记录 STW，真实栈处理在 zStackWatermark 家族 |
| `runtime/src/UnwindStack/StackExposureHook.h:1` | 删除 | 同上；诊断接口与专属 harness 同删 |
| `runtime/src/Heap/WCollector/UntagRefFieldBreadcrumb.h:1` | 删除 | TLS breadcrumb 的信号打印接口，非 ZVerify |
