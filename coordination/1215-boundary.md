待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
LANE=sym_cangjie_runtime_1215_implement_r5855214347
问题：#1215 按消费者删除的边界与普通调用点分类。
实测源码：冻结 cbadab05282e6ac93a33e0eeeee6a55e62373f90 中 PrologueRegisterClosure::RecordCalleeSaved 有两条产品消费者：StackFrameCursor.cpp:208（GC）及 StackGrowStackInfo.cpp:73（移动栈，同时消费 gcRegRoot/stackPtrRegRoot）。后一条不只服务 GC callee-saved 根，整体删除将改变栈指针搬移。
拟保留 StackPtrMap 的 prologue 消费，仅删 RootMap 持有的 calleeSavedPrologue 与 RecordCalleeSaved、GC 在 managed 帧后的保存位置传播；保留共享解码格式。
普通调用点目前 FrameInfo 只有 MANAGED，没有栈图 site-kind；合法 poll/stackgrow 的寄存器来自紧邻更年轻 SAFEPOINT/STACKGROW 桩，return 专用 map 独立消费。拟在普通 managed 帧消费前使用前一桩类型判定，合法桩只覆盖紧邻一个 managed 帧，跨普通 managed 帧不得传播寄存器保存位置。请确认此分路是否为获准边界，或指定必须使用的已有 site-kind 来源及 HotSpot 同形锚。
ZGC/HotSpot 锚：cpu/x86/frame_x86.inline.hpp:455-464 编译帧不更新 callee-saved map；share/runtime/frame.cpp 的 oopmap 消费仍使用 blob 提供的 register map。
同时请给 LLVM#87 合入后全量重编的染色 std/编译器装置坐标，以完成本 issue 要求的 Opus 同装置真实输入预演（不擅自复用旧 ABI 或改共享 SDK）。LLVM#87 已 CLOSED，Delivery 2869dc4cb8a1868e1cb3bf9d3e5ef59cb79d13b6；仅此不构成重编装置已具备的证据。
PROGRESS=WIP，继续不依赖答复的消费者和测试装置盘点。
