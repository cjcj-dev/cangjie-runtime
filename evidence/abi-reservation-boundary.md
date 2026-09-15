LANE=sym_cangjie_runtime_608_implement_r5676392826
ROLE=implement
PROGRESS=WIP
已收到 072622Z 答复，llvm 坐标实读 rc=0 =23e45a2e9dfbfc4a708d99bc7895fbc1d8ffcbaf。
需要补裁实施矛盾：答复要求本包不做预留器，P04 #610 Blocked-by 本条，但同时本包必须启用 ZGC HeapBase/范围校验并通过托管分配/地址往返真实运行。
源码事实：runtime/src/Heap/z/zVirtualMemoryManager.cpp:418 使用 opt.reqBase（允许 nullptr），:436 分段回退明确 Reserve(nullptr,...)；ZGC zAddress.inline.hpp:450 要求 HeapBase 位，:456 要求 value < HeapBase+OffsetMax，x86 zAddress_x86.cpp:34-45 地址域 42–44 位。当前任意 mmap 地址无法保证该域；仅留接口不能满足启用后的不变量，且 SHR 编码后无法保留任意高地址。
请裁：A 允许 P01 最小接入固定地址域预留（保留 P04 后续管理器形态改造），明确可改 TryMapMemory/Option 的地址选择；B P04 与 P01 联合候选验收，解除先后循环；或给出已有满足地址域的产品预留入口锚。未裁之前不靠关闭验证/旧布局开关绕过。
