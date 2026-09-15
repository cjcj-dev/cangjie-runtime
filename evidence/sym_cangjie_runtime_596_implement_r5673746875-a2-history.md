LANE=sym_cangjie_runtime_596_implement_r5673746875
ROLE=implement
A2 返工源码确认：GcPhaseEnum C1/C4先PlainRootObject后PushHeapRoot；C2/C3先StripRootObjectColour再发布（Mutator.cpp:1018-1059）；PublishThreadRoot只把plain对象入队(zMark.cpp:1359-1368)。StackWatermark.hpp:18-24明确采用eager handshake；状态只有epoch/phase/cursor，没有prev_head_color/prev_frame_color。ZGC zStackWatermark.cpp:164-173,209-214明确携带历史色，zUncoloredRoot.inline.hpp:38-69先按历史色remap、mark再同槽heal。当前GCPhasePreForward(Mutator.cpp:1113-1189)是独立相位提前heal，无历史色判据。
这不是补一张表即可按「形态一致」闭合。拟在同包扩zStackWatermark.hpp/.cpp、栈扫描生产端和Mutator，按ZGC保存历史色+uncolored同槽barrier改C1-C4；范围超出原限定Mutator/FinalizerProcessor且需协调#603，故先报范围。
请裁：A 允许本包扩历史色生产/保存基础设施（具体到StackWatermark/StackFrameCursor等），按ZGC形态修复并测；B 现有eager全栈remap属于无return-statepoint基础设施例外，本包先实证current合同并给双方锚，历史色迁移另属#603；C 其他明确边界。不以「等价」收口，不猜current。
独立工作继续：value/export已有真实CompactRegion重叠from/to测试(clear_entries_product_unit.cpp:2923-3010)，正核生产者与OverwritePrevious时序。
