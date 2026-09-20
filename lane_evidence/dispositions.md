# 追加清单逐项处置（源码锚基于 f1b274e4ca3a56e75af36cec7969d207420cef84）
待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
| 类 | 用例 | 处置 | 锚 |
|---|---|---|---|
| P-H | MarkPort203Entries.FirstLiveIsAccountedAfterCacheFlush | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:57 |
| P-H | MarkPort203Entries.RepeatedStrongClaimDoesNotAccountTwice | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:62 |
| P-H | MarkPort203Entries.FinalizableUpgradeDoesNotAccountTwice | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:67 |
| P-H | MarkPort203Entries.LargeFirstLiveIsAccountedAfterCacheFlush | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:72 |
| P-H | MarkPort203Entries.LargeRepeatedStrongClaimDoesNotAccountTwice | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:77 |
| P-H | MarkPort203Entries.LargeFinalizableUpgradeDoesNotAccountTwice | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:82 |
| P-H | MarkPort203Entries.CacheCollisionAndExitWriteBothPageCounts | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:87 |
| P-H | MarkPort203Entries.SerialCollectionConsumesArrayTails | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:415 |
| P-H | MarkPort203Entries.LegacyParallelCollectionConsumesArrayTails | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:419 |
| P-H | MarkPort203Entries.StripedCollectionConsumesArrayTails | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:423 |
| P-H | MarkPort203Entries.MajorSerialCollectionConsumesArrayTails | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:430 |
| P-H | MarkPort203Entries.MajorParallelCollectionConsumesArrayTails | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:434 |
| P-H | MarkPort203Entries.MajorExportCollectionConsumesArrayTails | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:438 |
| P-H | MarkPort203Entries.SerialInvisibleRootIsLiveWithoutFollowingFields | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:447 |
| P-H | MarkPort203Entries.StripedInvisibleRootIsLiveWithoutFollowingFields | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:451 |
| P-H | MarkPort203Entries.FinalizableArrayClosureAccountsWithoutStrongUpgrade | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:458 |
| P-H | MarkPort203Entries.SerialCollectionHandlesExactArrayThreshold | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:465 |
| P-H | MarkPort203Entries.SerialCollectionHandlesOnePastArrayThreshold | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:469 |
| P-H | MarkPort203Entries.StructArrayCollectionVisitsBothFields | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:476 |
| P-H | MarkPort203Entries.SerialInvisibleThenNormalAccountsOnce | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:483 |
| P-H | MarkPort203Entries.SerialNormalThenInvisibleAccountsOnce | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:487 |
| P-H | MarkPort203Entries.ParallelInvisibleThenNormalAccountsOnce | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:491 |
| P-H | MarkPort203Entries.ParallelNormalThenInvisibleAccountsOnce | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:495 |
| P-H | MarkPort203Entries.StripedInvisibleThenNormalAccountsOnce | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:499 |
| P-H | MarkPort203Entries.StripedNormalThenInvisibleAccountsOnce | 保留产品 livemap/数组尾部断言；真实相位夹具交接由本包修正 | runtime/tests/gc_unit/test_mark_port_203_entries.cpp:503 |
| P-I | RawRemapYoungProduct.MajorRemapsStackObjectField | 已由前置包迁产品槽/转发结果断言；保留 | runtime/tests/gc_unit/clear_entries_product_unit.cpp:1431 |
| P-I | RawRemapYoungProduct.MajorRemapsHeaderlessRecordField | 已由前置包迁产品槽/转发结果断言；保留 | runtime/tests/gc_unit/clear_entries_product_unit.cpp:1435 |
| P-I | RawRemapYoungProduct.MajorWatermarkConsumesYoungTable | 已由前置包迁产品槽/转发结果断言；保留 | runtime/tests/gc_unit/clear_entries_product_unit.cpp:1439 |
| P-I | RawRemapYoungProduct.MajorWatermarkConsumesPromotedYoungSource | 已由前置包迁产品槽/转发结果断言；保留 | runtime/tests/gc_unit/clear_entries_product_unit.cpp:1443 |
| P-I | RawRemapYoungProduct.MajorKeepsOldRawRootUntilNextRootScan | 已由前置包迁产品槽/转发结果断言；保留 | runtime/tests/gc_unit/clear_entries_product_unit.cpp:1447 |
| P-I | RawRemapYoungProduct.MajorWatermarkRemapsDerivedYoungSource | 已由前置包迁产品槽/转发结果断言；保留 | runtime/tests/gc_unit/clear_entries_product_unit.cpp:1452 |
| P-I | RawRemapYoungProduct.MajorWatermarkRemapsDerivedPromotedSource | 已由前置包迁产品槽/转发结果断言；保留 | runtime/tests/gc_unit/clear_entries_product_unit.cpp:1456 |
| P-I | RawRemapYoungProduct.MajorKeepsOldDerivedRootUntilNextRootScan | 已由前置包迁产品槽/转发结果断言；保留 | runtime/tests/gc_unit/clear_entries_product_unit.cpp:1460 |
| P-I | RawRemapYoungProduct.MajorFallbackRemapsDerivedPromotedSource | 已由前置包迁产品槽/转发结果断言；保留 | runtime/tests/gc_unit/clear_entries_product_unit.cpp:1464 |
| P-I | RawRemapYoungProduct.MajorFallbackKeepsOldRootUntilNextRootScan | 已由前置包迁产品槽/转发结果断言；保留 | runtime/tests/gc_unit/clear_entries_product_unit.cpp:1468 |
| P-J | ThreadRootCurrent.SavedColorNativeFrameRoot | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:296 |
| P-J | ThreadRootCurrent.SavedColorInvisibleRoot | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:297 |
| P-J | ThreadRootCurrent.SavedColorDirectNativeFrameRoot | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:298 |
| P-J | ThreadRootCurrent.SavedColorDirectInvisibleRoot | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:299 |
| P-J | ThreadRootCurrent.OrdinaryRootRoutesByTargetGeneration | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:301 |
| P-J | NativeRootCurrent.MinorPublication | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:345 |
| P-J | NativeRootCurrent.MajorSeed | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:346 |
| P-J | NativeRootCurrent.ColoredAndNullBoundary | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:383 |
| P-J | NativeRootCurrent.YoungGoodMarksBeforeHealingAndSkipsRepeat | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:399 |
| P-J | ThreadRootCurrent.C1StackFieldHistoricalColor | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:430 |
| P-J | ThreadRootCurrent.C2ObjectRefHistoricalColor | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:431 |
| P-J | ThreadRootCurrent.C3InvisibleHistoricalColor | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:432 |
| P-J | ThreadRootCurrent.C4HeaderlessHistoricalColor | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:433 |
| P-J | NativeRootCurrent.StrongFinalizerRootPublishesAndMarks | 已由前置包迁真实相位/槽状态；保留 | runtime/tests/gc_unit/test_native_root_current.cpp:437 |
| P-K | YoungConc.PaintedObjectSkippedByShouldEnqueue | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:160 |
| P-K | YoungConc.SingleCurrentMarkSuppressesEnqueueForEitherClosure | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:175 |
| P-K | YoungConc.ExportRootRegistrationDoesNotMarkIncomingValue | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:189 |
| P-K | YoungConc.RemovingExportRootPublishesPreviousValue | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:206 |
| P-K | YoungConc.YoungToYoungWriteNotInRemset | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:225 |
| P-K | YoungConc.OldToYoungStillRecorded | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:260 |
| P-K | YoungConc.BulkWritePublishesSatbWithoutYoungRegions | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:285 |
| P-K | YoungConc.TraceStorePublishesPreviousYoungTarget | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:311 |
| P-K | YoungConc.IdleStoreDoesNotPublishMarkWork | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:338 |
| P-K | YoungConc.StackScanIsRequired | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:362 |
| P-K | YoungConc.MarkEndDomainContainsPublishedYoungWork | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:371 |
| P-K | YoungConc.StoreBufferFlushPublishesYoungMarkWork | 产品发布栈/槽状态仍有观测点；保留，机制归 #720 | runtime/tests/gc_unit/test_young_conc.cpp:387 |

P-I 的 MajorDispatchRemapsLiveRemoteArrayField 在当前基线已经删除；本轮不重复删除，也不声称此删除由本包实施。
旧 remap 收据、zRelocate 门内 ForwardTask/WaitEnterHook 已由前置包清理；剩余 mark closure observer 消费仅在 test_segmented_array_init.cpp，归前置 #730，等待合入。
