待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`

坐标基于 `50a83f258c431e97a141c4e34e7c1239dd4e91ad`。供主控落位 `/root/cj_build/ops/design/ZGC_MECHANISM_LIST.md` 基础设施差异表；当前 stack-grow-on 候选已实现，尚在验收；按 #1213 主控裁决，stack-grow-off 验收与解码布局统一待 LLVM#83 合入后同分支补。

| 机制 | ZGC 形态 | 我方 | 判定 | 锚 |
|---|---|---|---|---|
| String 值类型经 sret 写入调用者栈帧 | Java 返回值经寄存器；暴露帧前处理水位；起始处理 callee/caller/unwind margin 三帧 | Cangjie sret 可把当前引用写入更高调用者帧；存活栈内指针可跨越三帧。需水位覆盖指向帧，并保证映射全构型发射 | 基础设施差异：String 值类型的 sret ABI；runtime#1213，生产映射配对 cjcj-llvm#83；未验收 | HotSpot cpu/x86/sharedRuntime_x86_64.cpp:635-645；share/runtime/stackWatermark.cpp:205-220；share/runtime/stackWatermark.inline.hpp:70-83；我方 runtime/src/Heap/z/zStackWatermark.cpp:154-167 |
