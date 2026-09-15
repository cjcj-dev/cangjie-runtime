# B19 A2 输入合同（WIP）

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标基于 `4f9a173ed86a77cd72de1863277c75e6652f8379`。
本页是实现输入及证据索引，不是自审放行。未取得资格的面不交给 #577 P3 猜测。

## producer → 保存 → consumer → 恢复

| 面 | 我方路径（runtime/src/） | ZGC（/root/cj_build/reference/jdk/src/hotspot/share/gc/z/） | 当前资格 |
|---|---|---|---|
| C1 栈对象字段 | Mutator/Mutator.cpp:1018 从真实 RootSlot 读取；:1020 剥色；:1021 发布 | zStackWatermark.cpp:209 保存的帧历史色 → zUncoloredRoot.inline.hpp:38 读同槽、:49 resolve、:52 mark、:59 heal | 尚未证明；原槽历史色未传入消费端 |
| C2 ObjectRef | Heap/z/zRootsIterator.cpp:268–275 stack map 访问真实寄存器保存槽/栈槽 → Mutator.cpp:1038 strip → :1040 发布 | 同上；历史色不是当前全局色 | 尚未证明；strip 在 mark 前写槽 |
| C3 invisible | Mutator.cpp:946/978 VisitRawObjects → :1056 strip → :1059 follow=false | zStackWatermark.cpp:164–173 prev_head_color → process_invisible；zUncoloredRoot.inline.hpp:79–80 DontFollow | DontFollow 已保留；历史身份仍未取得资格 |
| C4 headerless | Mutator.cpp:897–904 memcpy record+0 → plain 发布；preforward :911–928 对同字段 heal | zUncoloredRoot.inline.hpp:38–59 同槽屏障；String 值布局无直接 HotSpot 对应 | 布局是基础设施差异；不据此免除历史色证明 |
| value/export 新输入 | zHeap.cpp:392 GetExportObject → zRootsIterator.hpp:106 ReadStaticRef → zHeap.cpp:404 ResurrectExportObject → zMark.hpp:335/341 IncomingNew | zUncoloredRoot.inline.hpp:62–69 load-good 保持身份 | 有生产者 load barrier；CrossAccessBarrier :397–401 后续 ForwardObject 仍需核重叠键 |
| value/export 存储 | zMark.hpp:452–471 ValueRoot 保存 object/stage/color/generation；Stage() 按所属代色失效 → zGeneration.cpp:805/812 resolve | zUncoloredRoot.hpp 容器色；zUncoloredRoot.inline.hpp:62–69 历史色决定 remap | 必须实测同值在 IncomingNew/OverwritePrevious 两种身份下的不同结果 |
| value/export 消费 | zGeneration.cpp:826–850 set/map 重建并以 IncomingNew 保存结果；zRelocate.cpp:335–347 preforward 消费 | zUncoloredRoot.inline.hpp:49–59 resolve 后处理再保存 | 已有真实 CompactRegion fixture；运行证据待补 |

## 线程时序边界

Mutator.cpp:306–340 的 epoch ACK 先 GcPhaseEnum 后 release 完成；DrainStackWatermark :932–1005 在 MutatorLock 下扫描，GC owner 要求 saferegion。zStackWatermark.hpp:18–24 明示 eager handshake，:188–199 状态是 epoch/phase/cursor，不能把 epoch 数字当成历史颜色。
GCPhasePreForward :1113–1189 在独立 REMAP 相位写回各槽；同槽已处理集合用于重复访问。它不提供 ZGC prev_head_color/prev_frame_color 生产证据。
历史色基础设施是否纳入本轮已于 advisor 报告，等待裁决期间保持 WIP。

## 重叠键不变量与已有构造

clear_entries_product_unit.cpp:2923–2985 用真实 RegionManager::CompactRegion 构造 first.from=start+size → first.to=start，second.from=start+2*size → second.to=start+size。故 second.to 与 first.from 地址相同但对象身份不同。
IncomingNew(second.to) 必须保持 second.to；历史根 second.from 经所属代 relocation 色翻转必须变为 second.to。无关 young 色翻转不能改变已 current 的 old 地址身份。
现有9项 ValueRootCurrentization 覆盖 minor/major、重叠/非重叠/外页、无关代翻色、存储根 old 翻色；不能用通过数替代每项结果，也不能把直接调用 collector 的 fixture 外推到 CrossAccessBarrier 的资格。

## FALSIFIED

“旧报告 stack/value scanner 基础设施说明已包含逐项历史色合同”不成立，前轮 review R1 已明确指出。
