# P01 ABI 实施契约
待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

Runtime 基线 `3140f19160afc759ae540e8cc21cb323905d8cc9`；LLVM 基线 `23e45a2e9dfbfc4a708d99bc7895fbc1d8ffcbaf`。

## 已裁范围
072930Z 答复 A：P01 实施连续与分段回退的最小合法地址域预留；P04 保留管理器改造。Finalizable 生产/分类/掩码属于 P01，P2 后续接 slow/color。启用校验前必须真实输入预演。

## 编码表
| 项 | x86 | AArch64 | ZGC 锚 |
|---|---|---|---|
| 元数据 | rr 位 4–5、FF 位 6–7、mm 位 8–9、MM 位 10–11、RRRR 位 12–15 | 同左 | share/gc/z/zAddress.hpp:138 |
| remap_bits | 与 RemappedMask 做 AND | 先 XOR RemappedMask 再 AND | cpu/{x86,aarch64}/gc/z/zAddress_* .inline.hpp:29 |
| load_shift_lookup | remap index 查表，13/14/15/16；null 为 24 | 固定 16 | cpu/{x86,aarch64}/gc/z/zAddress_* .inline.hpp:33 |
| color | addr 左移 lookup(color) 后 OR color | 同左 | share/gc/z/zAddress.inline.hpp:734 |
| uncolor | ptr 右移 lookup(ptr) | 同左 | share/gc/z/zAddress.inline.hpp:609 |
| offset→地址 | offset OR HeapBase | 同左 | share/gc/z/zAddress.inline.hpp:599 |
| raw null | 只供未初始化存储 | 同左 | share/gc/z/zAddress.inline.hpp:327 |
| color_null | StoreGoodMask OR RememberedMask | 同左 | share/gc/z/zAddress.inline.hpp:750 |

以上 ZGC 路径根为 `/root/cj_build/reference/jdk/src/hotspot/`。

## 已读真实调用顺序
相位入口 → 全局 flip → set_good_masks → runtime/compiler 谓词 → load uncolor 或 store color → HeapSlot。
地址域初始化 → 连续/分段限定域预留 → RegionSpace 地址 → color；元数据 map 使用独立 native 地址，不能把所有 MemMap 限制到 heap 域。

## 删除与接入登记
EpochColours/BadMasks/ComputeBadMasks、Collector 谓词层、WCollector 掩码状态与 flip、HeapSlot 48 位字段、MakeStoreGoodSlotWord/ColourStoreGood/ColourLoadGood、ColourPredicates 参数层与 current_* 均需随消费者迁移删除。
LLVM load/store 两端必须同时使用新移位 ABI；store null 必须经统一 colored null 规则。禁止把相位当前 shift 用于 stale pointer 解码（须 lookup(ptr)）。

此文为实施准备，不代表已完成产品修改或验证。
