PROGRESS=WIP · verdict=两仓中间态已保存，容量暂停；P10线程身份与607函数会合待裁 ｜尺=源码迁移与git保存 · LANE=sym_cangjie_runtime_614_implement_r5687433872
DELIVERY_REF=cangjie-runtime|sym/614-implement-r5687433872|74e0de14e035d1ecbbf8af460575d96f6435577e
SIDE_EFFECT: 本棒runtime候选提交、LLVM稀疏候选提交及runtime draft PR#647；未改共享安装
ROLE=implement
EVIDENCE=local:/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_614_implement_r5687433872/evidence/p08

LANE=sym_cangjie_runtime_614_implement_r5687433872
ROLE=implement
PROGRESS=WIP

## 坐标回读
2026-09-16 开工：候选 HEAD=b38fcfdef926f6ae5dff2e8d0ffa8f4d6356bc18，branch=sym/614-implement-r5687433872；git status --short 输出空，命令 rc=0。
指定主树 git -C /root/cj_build/cangjie_runtime rev-parse cjcjdev/main 输出 91f3dcc232201d4ad98ec3af6f10165edb950416，rc=0。主线已前进，交付前按任务书 fetch+merge 并核内容。

## 进展
已读 recovery/README.md、文件地图相关屏障/TLS/buffer 映射及 overlap 公共 API 所有权。尚未改产品；尚未构建或测试。LLVM 联合交付需要其明确坐标；容量状态核实后再执行复制/构建。

## FALSIFIED
无。

## 20260915T2020Z 保存点（不是交付）
- fetch+merge cjcjdev/main 实际rc=0，快进到91f3dcc232201d4ad98ec3af6f10165edb950416；保留P02/P06。
- 保存提交 2f1bb137974c6848ba10fe6180176e65d09a054c：删原始类型WriteI8..F64/WriteField及Windows旧导出；MObject/MArray直接Field::SetFieldValue；删ReadPlainRoot/WritePlainRoot并将所有消费者改既有typed plain load/store。无色根GC处理和TSAN同步保留。本提交未构建，不能标作可交付。
- producer→consumer与完整rg输出已在工作树evidence/p08/。git diff --check rc=0只检查补丁空白，不是编译或行为证明。
- 主控201455Z答复明确：10.9GB恢复回执已被20:03Z空间再次耗尽覆盖，继续暂停大复制/构建；LLVM坐标及#607会合仍待root裁定。

CLAIM: ZGC实际slow/buffer包含相位判断，任务书字面零相位判据不能与逐函数形态同时成立
  METHOD: read
  EVIDENCE: /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zBarrier.cpp:61; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStoreBarrierBuffer.cpp:190; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStoreBarrierBuffer.cpp:217

## FALSIFIED 补充
- 不变量②字面“屏障/缓冲路径GCPhase消费为0”：上条ZGC读证反例；已提交advisor 201638Z，未获裁定前不据此删必要slow/buffer语义。
- 较早容量恢复不能作为当前开建依据：主控201455Z答复已明确被后续空间耗尽覆盖。

## 尚未执行
UNIT_DEFAULT_RC=NOT_RUN(主控容量暂停，未构建)
UNIT_FILLER_RC=NOT_RUN(主控容量暂停，未构建)
UNIT_OHOS_RC=NOT_RUN(主控容量暂停，未构建)
统一漏斗/BarrierSet/Runtime/TLS/buffer/LLVM仍未完成；尚无产品红臂或联合bundle。本报告保持WIP，不送审。

## 20260915T2032Z 保存点（本节优先）

- advisor 201638Z已接受两项反证：slow/buffer保留ZGC相位/代判断；install_base_pointers两个调用入口按颜色防重。原相位零计数及单入口句子不再作为验收要求。LLVM冻结主树/root/cj_build/llvm_rebase的main/cjcjdev/main均1a01451912f160219665abdc497274e574338bad，实际回读rc=0。
- LLVM独立稀疏树：/root/cj_build/llvm_rebase_wt/sym_cangjie_runtime_614_implement_r5687433872；分支sym/614-p08-llvm-r5687433872；保存5ba7705a1a5da3ec7fccee381527a7a88cf7e64e。只检出规则与CJBarrierLowering.cpp，未大复制SDK/LLVM。删GCPhaseCheck/EnableGCPhase/GetGCPhase声明生成，保留P01的null-owner+proven-nonheap分类与disabled-GC原始写。尚未编译，其他快路/ABI未完成。
- runtime 74e0de14e035d1ecbbf8af460575d96f6435577e：ThreadGCData改为5掩码字段、store-buffer指针、两代内嵌mark stacks与invisible-root指针；直接消费者和三份既有测试仅适配字段访问。生命周期惰性分配、TLS masks生效与invisible root迁移尚未改，等待P10线程身份裁定。本中间态不是TLS完整交付。
- runtime draft PR#647保存到先前2f1bb1379；它不代表当前本地HEAD已发布。201638Z要求后续由独立publisher/merge发布，收到后未再推送。
- 已读kkk2容量观测（box rc0）：20:21Z df显示可用1.3G、100%；uptime load0.19/0.59/1.45。仅一次资源观测，不能推产品性质。主控暂停仍生效，未建测。
- 尚待202355Z：P08/P10 phase-update入口与OS线程/协程身份；#607共享函数迁移边界。获准独立本地实现继续进行，不覆盖#607。

CLAIM: TLS数据保存点已将mark stacks从unique_ptr改成线程内嵌对象，消费者访问同一对象
  METHOD: read
  EVIDENCE: runtime/src/Heap/z/zThreadLocalData.hpp:22; runtime/src/Mutator/ThreadLocal.cpp:51; runtime/src/Heap/z/zMark.cpp:1959
CLAIM: LLVM保存点保留P01已证明非堆槽写路径，删除依GC相位生成的屏障省略装置
  METHOD: read
  EVIDENCE: /root/cj_build/llvm_rebase_wt/sym_cangjie_runtime_614_implement_r5687433872/llvm/lib/CodeGen/CJBarrierLowering.cpp:286

SYM-PR: cangjie-runtime#647
