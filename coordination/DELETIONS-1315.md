待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标基于 `9733dfc09d29eca27d19cc3937a149838b4e3322`；候选 `4cd43656c25929f9f4c3d96ea0fca0cc0e7a0048`。

| 删除符号 | 基线命中行 | 候选命中行 |
|---|---:|---:|
| RegionLifeId | 18 | 0 |
| page_life_id | 4 | 0 |
| page_life_current | 3 | 0 |
| BumpRegionLifeId | 3 | 0 |
| GetRegionLifeSeq | 1 | 0 |
| regionLifeSequence | 4 | 0 |
| forwarding_for_page | 18 | 0 |
| generation_forwarding_table | 6 | 0 |
| generation_relocate_queue | 6 | 0 |
| forwarding_find | 3 | 0 |
| _retireHook | 5 | 0 |
| safeDestroy | 7 | 0 |
| RetirePageMemory | 6 | 0 |
| WaitCopiedBeforePayloadWipe | 3 | 0 |
| ResetFlipPromotedPages | 3 | 0 |
| GetGhostRegionSize | 2 | 0 |
| CopyInflightWord | 1 | 0 |
| PeekForwardingOwner | 1 | 0 |
| DestUsable | 1 | 0 |
| IsCompacted | 2 | 0 |
| IsRoutingState | 2 | 0 |
| RelocateObserve | 2 | 0 |
| RetainScope | 9 | 0 |
| InPlaceClaimScope | 10 | 0 |
| fwdOwner | 2 | 0 |
| retiredLivemap | 4 | 0 |
| ownerRegion | 2 | 0 |
| copyInflight | 2 | 0 |
| ghostLifeId | 1 | 0 |
| SumAllocatedByRoles | 5 | 0 |
| ReclaimRetiredRegion | 3 | 0 |
| ReleaseRetiredRegion | 3 | 0 |
| ZPageRole::From | 6 | 0 |
| ZForwarding::Create | 0 | 0 |

所有命令、rc 和逐文件输出见 `evidence/deletions.json`。`Create/Destroy` 的类内声明用下方完整差分及唯一 alloc 签名检索核对；限定名基线可能为零，不能单独充当删除证据。
