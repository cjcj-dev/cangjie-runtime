待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标基于 `cbadab05282e6ac93a33e0eeeee6a55e62373f90`。

# 本棒实现前边界记录（非当前真值）

| 路径 | 源码锚 | 输入来源 | 处理 |
|---|---|---|---|
| GC 持久水位 | Heap/z/zStackWatermark.cpp:661 → UnwindStack/StackFrameCursor.cpp:20 → :179 | cursor.RegMap | 普通调用点守卫须早于 :201 derived 消费和 :203 register 消费 |
| eager GC | UnwindStack/GcStackInfo.cpp:121 | VisitStackRoots 局部 map | 与持久水位同分路 |
| eager heap roots | UnwindStack/GcStackInfo.cpp:169 | stub 保存位置 | 当前绕过 ProcessFrame，直接 ProcessManagedFrame，不能漏掉 |
| diagnostic root | UnwindStack/GcStackInfo.cpp:228 | RecordStackInfo::stacks | 同上 |
| ARM eager | UnwindStack/GcStackInfo.cpp:24,45,87 | 架构特定 stub 保存位置 | 不能只改非 ARM 分支 |
| return values | UnwindStack/StackFrameCursor.cpp:81,121 | 返回专用寄存器 map | 保留 |
| stack relocation | UnwindStack/StackGrowStackInfo.cpp:55,73 | gcRegRoot 与 stackPtrRegRoot | 共享 prologue 非 GC-only，待范围裁决 |
| sret closure | Heap/z/zStackWatermark.cpp:167-186 | incoming map 复制 + StackPtrMap::VisitReg | 刚入主线 #1214；不能静默回退 |

HotSpot 锚：cpu/x86/frame_x86.inline.hpp:455-464（compiled frame 不传播 callee-saved map）；share/compiler/oopMap.cpp:496-499（stub 保存位置消费）；share/gc/z/zStackWatermark.cpp:209（按帧处理）。

# 测试处置预案

- `StubRegisterRoots.GprSlots/XmmSlots/BitmapPositions`：保留；返回点不是普通调用点。
- `StackMapBaseCapture.*`：保留 derived/base 的顺序断言；共享 decoder/header 仍服务合法 stub。
- `RelocateStartFrameRoot.WritesToAddressBeforeConcurrentRelocate` 与 `StackGrowCopy.HealsFrameRootBeforeCopy`：现有夹具用普通帧 prologue R13 根。不能删重定位/复制断言；应改为本帧槽根夹具，保留 before/after 目标断言。
- `SretWatermark.UsesIncomingRegisterPointerMap`：等待生产端/消费者边界裁决；不可直接删除或弱化。
- 新增三帧输入应覆盖：全保存桩→合法 managed 寄存器 map→普通 caller 寄存器 map（精确 fatal）；对应第三帧本帧槽根通过；SAFEPOINT/STACKGROW/RETURN_SAFEPOINT 各有合法对照。
- fatal 断言需读取 fatal 信息并校验信号；单纯异常退出不能判目标命中。生产端切刀：改变桩保存位置来源使合法对照目标槽值红；消费端切刀：移除 ordinary-call guard 使专用拒绝断言红。至少一刀通过真实 phase entry 的产品调用。

# 运行前条件

kkk2 2026-09-27 18:58:24 uptime 17 days 19:06，load=528.42/387.22/268.66，磁盘可用 84G。尚未启动构建。load>150 时应降低构建并发并记实际值。
LLVM#87 已合入 aa4171e6ac97e8b90e50fb60e6dda0bfd755be66（PR#89 mergedAt 2026-09-27T10:53:30Z）。其 PR 说明仅 LLVM 半边完成，不证明新 tuple 全量重编装置已齐。

# 生产端继续核对（等裁决期间）
读取坐标来自 LLVM#87 交付报告 `/root/cj_build/reports/REPORT-sym_cjcj_llvm_87_implement_r5854773255.md`：`/root/cj_build/llvm_rebase`；实际 rev-parse cjcjdev/main = aa4171e6ac97e8b90e50fb60e6dda0bfd755be66，rc=0。
`llvm/lib/CodeGen/SelectionDAG/StatepointLowering.cpp:575-579` 的 lowerStackValue 使用 lowerToSpillSlot；`LoweredStackPtrs` 的文件内命中仅 :575,576,577,689,873，未看到向此集合插入的生产动作。不能因此直接删除 runtime 的 SPRegIdx：`llvm/lib/CodeGen/StackMaps.cpp:785-791` 的 parseCangjieStackOpers 仍接受 register；:829-836 按 MIR 栈指针数量读取；:1491-1497 仍发出 SPRegIdx。需要界定真实生产变体，不能从集合名或一次 grep 推断特性不存在。
