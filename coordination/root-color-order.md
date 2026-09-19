# 第五处 producer → consumer（修改前坐标 bfc976ae966e5dd9b82cbab3c86d47c5ac01c4f6）
待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

| 顺序 | 产品位置 | 状态 |
|---|---|---|
| Attach 保存线程当前颜色 | zThreadLocalData.cpp:71 | 根与 loadGoodMask 同代 |
| GcPhaseEnum 创建扫描闭包 | Mutator.cpp:843 | 必须在 InstallMasks 前捕获根保存色 |
| process_head | zStackWatermark.cpp:76 | 扫描非帧根 |
| InstallMasks | zStackWatermark.cpp:99 | 更新线程屏障色，帧扫描闭包必须仍持旧色 |
| process / ProcessFrame | zStackWatermark.cpp:121 | 扫描帧内根 |
| PushHeapRoot → mark/process_invisible | Mutator.cpp:791,793 | 消费捕获的旧色 |
| make_load_good → relocate_or_remap | zUncoloredRoot.inline.hpp:24-29 | 依根保存色选择 forwarding |

ZGC 锚：zStackWatermark.cpp:155-159 闭包按值保存色；:164-173 head 使用 previous color；:210-211 frame 使用 previous color；zUncoloredRoot.inline.hpp:62-67 根据该色 remap。
我方基础设施：无 return statepoint，当前 finish_processing 在同次锁定扫描中完整处理所有帧；尚无分段恢复，因此本轮闭包保存扫描开始的线程色，不能在帧回调时读取已更新的线程色。
