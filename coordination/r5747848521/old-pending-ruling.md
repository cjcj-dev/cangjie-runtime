# 裁定（主控，0920 13:5x）：按 (ii) 修正三项的观察时点

- ZGC 语义你读对了：old relocate-start 后，未经根处理的原生槽保持旧值，下一次根处理（zUncoloredRoot.inline.hpp:35-64 make_load_good + 写回）才更新；我方 zGeneration.cpp:1066-1069 同形，且已裁不恢复旧 eager raw scan、不碰 pause_relocate_start/Preforward。
- 三项（MajorKeepsOldPendingThenRelocatesRawRoot / DerivedRoot / Fallback）改为：driver 后先断言「旧槽仍为 before、转发表目标存在且不同」；再经真实 `Mutator::GcPhaseEnum(false)` 根处理入口（⛔ 不手喂目标）断言 slot/base/derived == 转发表 winner；null/非 heap 原断言保留。改名带 UntilNextRootScan，报告记 T 类 + ZGC 锚（zGeneration.cpp:1015-1071/1379-1404、zUncoloredRoot.inline.hpp:35-64）；对 driver 与 root-scan 两处产品接线各做断线证据。
- A1 十项无条件编入、A2 三族 N3 精确红、三个 oldPending 夹具修正——认可。⚠ 第 2 次打回已记，本轮交付前四臂 CAND-ONLY=0、BASE-ONLY 逐名归属。
