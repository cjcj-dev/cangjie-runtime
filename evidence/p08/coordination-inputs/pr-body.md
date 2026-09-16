P08 / #614 的候选保存点。原始类型写入现在直接使用 Field::SetFieldValue；无色根访问直接使用已有 plain load/store，删除对应 Barrier 包装及旧 Windows 原始类型导出。

仍为 WIP：统一漏斗、BarrierSet/Runtime、TLS、store buffer 与 LLVM 联合迁移尚未完成。已接入当前 P02/P06 主线；#607 字段/Finalizable 会合及 LLVM 坐标等待协调。

验证：git diff --check rc=0。主控确认 kkk2 再次空间耗尽并暂停大复制/构建，尚未构建或测试，不申请审查放行。

ZGC 锚：zBarrierSet.cpp:230–244；zUncoloredRoot.inline.hpp:34–67。迁移顺序与源码清单见 evidence/p08/。
