# 改产品前：managed 引用存储发射对照
待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标：runtime 779924ea4ca8d91e2f1fd049410f17e419d6eb8b；只读 llvm_rebase b82ea71361caecd6ff9b1505ec2799280bdd8079；只读 cjcj 26a8a027e67e2ba9e68f4bca80b2d84b7c13a45e。仅证明这些源码的分支，不推断负载所用 SDK 构型。

| managed 来源 / 分路 | 实际发射源码 | ZGC 对应 / 差异 |
|---|---|---|
| 引用数组元素、引用值赋值 | /root/cj_build/cjcj/packages/codegen/src/ArrayImpl.cj:760；IRBuilder.cj:1503,1523,3596-3615：value/base/place 转为引用类型，生成 llvm.cj.gcwrite.ref | 前端给字段地址和新值；未在此前端函数发射 ZGC 三级机器码 |
| LLVM pass 调用顺序 | /root/cj_build/llvm_rebase/llvm/lib/CodeGen/CJBarrierLowering.cpp:925-926：writeBarrierFastPath 后 doLowering | 必须查调用层，不能只看 IntrinsicMap |
| EnableTaggedPointer && !CangjieJIT、gcwrite_ref、base 不是常量 null | 同文件:644-658 调用 storeFastPath；:519-539 读旧槽，读全局 g_cjStoreBadMask，按旧值与 mask 以及 owner/heap-slot 判定 | ZGC /root/cj_build/reference/jdk/src/hotspot/cpu/x86/gc/z/zBarrierSetAssembler_x86.cpp:428-470 fast；ZGC 非 nmethod :457-469 读 thread mask，而这里读 global |
| fast 命中 | LLVM 同文件:550-578，读全局 load shift、store good mask，着色后 volatile store，:580 将原 intrinsic 移至失败块 | 已存在内联 fast 分支；与 ZGC 形态仍不同，不判通过 |
| fast 未命中 / 未获 fast 资格 | LLVM 同文件:580,722-731,120,225：保留 intrinsic 并映射 CJ_MCC_WriteRefField 调用 | ZGC medium :508-562 非 atomic 缓冲槽及旧值，满才 slow；此 LLVM 分支直接到出线调用，缺该 medium 分路 |
| runtime 出线 | 本树 runtime/src/CommonAlias.h:12-13 alias → CompilerCalls.cpp:318-344 MCC_WriteRefField，heap-slot :333 → ZBarrier::WriteReference | 保留现有语义，P-A1 #843 仍未交付；本包不能提前声称其已收敛 |

## producer → consumer 与 ZGC 函数对应
| 阶段 | 本树 | ZGC | 判定 |
|---|---|---|---|
| 全局翻面 | runtime/src/Heap/z/zAddress.cpp:63-74 set_good_masks → PublishMasks | zAddress.cpp set_good_masks | 已有全局发布；不等同所有线程立即更新 |
| thread 数据与偏移 | runtime/src/Heap/z/zThreadLocalData.hpp:20-24,49-53 | zThreadLocalData.hpp:37-42,123-124 | 已有字段和 offsetof；本树 TLS gcData 是指针，不能宣称与 HotSpot thread 内嵌偏移相同 |
| thread 初次 attach | runtime/src/Heap/z/zThreadLocalData.cpp:57-74 | zBarrierSet.cpp:260 | 已安装 published masks |
| phase thread 更新 | runtime/src/Heap/z/zStackWatermark.cpp:162,183 finish_processing 内两处 InstallMasks | zStackWatermark.cpp:177-192 start_processing_impl，process_head 后、TLAB retire 前 | 分路点不同，(a)；必须裁决后按 ZGC 点改，不能以已有字段认定任务无效 |
| 外部消费者 | LLVM CJBarrierLowering.cpp:525 全局 mask | x86 assembler :457 TLS mask | 编译器侧内联发射属跨仓前置，本包只交 runtime 侧契约 |

## 删除清单（待裁决）
拟迁移 finish_processing 的两处 InstallMasks 到 start_processing_impl 的 process_head 后，删旧调用点。未发现需要另加 store_bad_mask 副本的依据。跨仓全局 mask 发射路径的替换不在本 runtime 包内实施。

## FALSIFIED
“所有引用存储均出线”的无条件源码断言不成立：LLVM storeFastPath 真实被调用且命中臂直接 store。是否部署启用须另有 SDK 身份证据。
“颜色翻面处一次刷新全部线程”不是所给 ZGC 的 thread 更新位置：ZStackWatermark::start_processing_impl 执行 thread mask 更新。
