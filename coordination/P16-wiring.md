待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标基于 `325d6ad73f99cc463a804aedd39595af05791c9a`。

# 修改前 producer → consumer 顺序
| producer | consumer | 修改位置与 ZGC 锚 |
|---|---|---|
| zGeneration.cpp:1158 old mark end | zVerify.cpp:73 AfterMark → Objects:222 | guarantee 应在 AfterMark，ZGC zVerify.cpp:497–504 |
| zGeneration.cpp:273 pause verify | zVerify.cpp:78 AfterWeakProcessing → Objects:222 | Objects 首先 should_abort，再线程处理，再 Heap 包装；ZGC zVerify.cpp:467–487 |
| zRelocate.cpp:1839 ForwardRegion | zVerify.cpp:291 BeforeRelocation → zPage.inline.hpp:407/409 | 页级 remset 清空守卫必须承接；ZGC zVerify.cpp:613–638、zPage.cpp:153–163 |
| zRelocate.cpp:1844 AfterRelocation | zVerify.cpp:365 AfterRelocation → AfterRelocationInternal:320 | to_age 入口守卫，ZGC zVerify.cpp:741–761 |
| zRelocate.cpp:1383 VerifyRelocatedPage | zForwarding.cpp:265 verify → zPage.cpp:135 verify_live | 移除 ZVerify::Object 公开额外检查；ZGC zForwarding.cpp:369–409 |

# 尚待 advisor 裁决
Allocator 活跃适配设施整删的接续边界；shared/stringdedup 布局与仅z目录规则冲突。
