# #700 standalone 身份守卫迁移

仅修改 `runtime/tests/gc_unit/run_standalone.sh`，不改产品、测试断言或执行方式。

| 旧检查对象 | 当前真实产品对象 | 处理 |
|---|---|---|
| RegionInfo::CloneForPromotion | ZPage::CloneForPromotion | 使用实际 SO full nm 核出的符号，迁入 STANDALONE_FULL_SYMBOLS |
| WCollector::MarkObject | ZMark::MarkEntryObject | 与已迁移的真实 page claim/live accounting 测试入口一致，迁入 full 检查 |
| Collector::MarkObjectIfActive | ZMark::MarkOldObjectIfActive | 使用当前 static 标记入口，迁入 full 检查 |

三项均先要求产品 SO 的 full nm 存在实际定义，再禁止测试 ELF 的 full nm 存在本地定义；保留原 main 阳性检查。两项 ZLiveMap 检查保持原范围。测试名无增删，变化为已删除旧类的身份检查对象迁移。

## 实际符号与产物身份

使用 `nm --defined-only`，未用 `nm -D` 判本地副本。default/testable 两次 nm rc 均为 0，均得到：

```
_ZN12MapleRuntime5ZPage17CloneForPromotionEv
_ZN12MapleRuntime5ZMark15MarkEntryObjectEPNS_10BaseObjectERKNS_14MarkStackEntryEPNS_13MarkLiveCacheE
_ZN12MapleRuntime5ZMark21MarkOldObjectIfActiveEPNS_10BaseObjectEb
```

保留产物（`/root/sym_cangjie_runtime_700_implement_r5723534433-shared-string/<arm>/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so`）：
- default sha256 `07691da14cdece890a5dac59a0184cb54048e47820503051eae5f26b086ae9a1`
- testable sha256 `e46a4aa307750aa27732ff1b7a97aa168ec3a9b9a4c1f200f4f503e803228e9b`

完整 full nm、rc、哈希和选中行分别存：
`kkk2:/root/sym_cangjie_runtime_700_implement_r5723534433-debug/identity-{default,testable}.{fullnm.txt,fullnm.rc,sha256,selected.txt}`。

## 上线前真实输入预演

从修改后的 runner 原样提取身份检查片段，脚本 `.lane-parallel/identity-preflight.sh`（远端同棒 debug 目录亦有）。输入为上述两构型真实 SO 的符号清单，以及保留的 379 两构型测试 ELF；这里只验证身份清单，不运行该旧 ELF，更不将两版本混合解释为行为验收。

| 输入 | ELF sha256 | main 阳性 | 守卫 rc | 被拦对象 |
|---|---|---|---|---|
| default | 86862ec7ace8da568b6d590da4982e83706687f96031d21f3726abc4a48ad91a | `000000000001d8e0 T main` | 0 | 无 |
| testable | 5dd4ec6521a074c082ce806f4bd4b7850bce8734e69e49699a347d44fa2913c7 | `0000000000025ff0 T main` | 0 | 无 |

阴性检查配套阳性：每个产品 SO 均实际定义三项被查符号，测试 ELF full nm 均实际含 main；不会以已退休名称恒不命中冒充有效检查。

预演日志：`kkk2:/root/sym_cangjie_runtime_700_implement_r5723534433-debug/identity-{default,testable}.preflight.{log,rc}`；输入哈希：同目录 `identity-preflight-{default,testable}/preflight-input.sha256`。bash -n 和 git diff --check 均 rc=0。新候选完整构建/三臂执行由父线程统一完成。
