# impl_zgc_minorconc_align: EpochHandshake 恒真后 stack-scan receipt 确定性为 0/0

LANE=impl_zgc_minorconc_align
ROLE=implement

## 实测红

冻结门在 candidate `fe7e29b060bf3aee0ad92d3fa67b5f8cf5ffced5` 的 build 段真运行 gc_unit；main runner 390/390 与 publication 15/15 已完成，但 finalizer trigger rc=134：

```
[GCV2][epoch-handshake] source=pre-minor-stack epoch=2 requested=1 acked=1 ... stack_scanned=0 stack_fallback=0
Check failed: stackScanEpoch != 0 && handshake.stackScanned + handshake.stackFallback == handshake.requested
minor concurrent stack scan accounting failed: epoch=2 requested=1 scanned=0 fallback=0
```

证据：`kkk2:/root/impl_zgc_minorconc_align_evidence/gate_v2_run/build.log` 与 `gate_v2.log`。

## 源码闭环

- 任务指定的 `EpochHandshakeEnabled()` 已删 `MRT_GCV2_EPOCH_HANDSHAKE` getenv 并恒真：`MutatorManager.cpp:270-275`。
- minor 因此无条件进入 `RunEpochHandshake("pre-minor-stack")` 并要求 receipt 计数等于 requested：`Generation.cpp:684-700`。
- 但 `Mutator::AcknowledgeEpochHandshake` 只在 `ConcurrentStackScanEnabled()` 为真时扫栈/记账：`Mutator.cpp:346-359`。
- `ConcurrentStackScanEnabled()` 仍受另一 getenv `MRT_GCV2_CONCURRENT_STACK_SCAN` 控制且默认 false：`MutatorManager.cpp:277-284`。
- 所以「Epoch handshake 已是 minor 唯一路径」与「stack receipt 仍是可选构型」在产品上矛盾，并非随机测试问题。

## 请裁决范围

ZGC 对齐不可保留「恒真 handshake + 可选 receipt」。请裁：

1. 是否允许本棒同批删除 `MRT_GCV2_CONCURRENT_STACK_SCAN` 并使 `ConcurrentStackScanEnabled()` 恒真（影响 minor + major）；或
2. 是否只将 minor 的 epoch stack scan 变为必需，保留 major 现状（需要给 handshake 增加显式 scan-roots 模式，不再从全局 getenv 读）。

我倾向 2：只收口 minor 用户令范围，避免静默翻 major 构型；但这会扩到 `Mutator.cpp/MutatorManager.{h,cpp}` 的 handshake 合同，需 controller 明确放行。
