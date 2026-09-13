# D09 #484 返工：定义三分与独立诊断边界

坐标基于搬前主线 `3439312e41310da5c961ffa2025642e19081142f`；返工起点 `5df4f90e0fce8a05b004a4470f1a63f58f0ed389`，候选仍在 `sym/484-implement-r5653028906`。

本表记录定义物理位置；函数体、调用、条件编译不改。`docs/gc_z_layout_functions.tsv` 是完整搬移身份表，其中历史 `zgc_file_anchor` 仅为文件职责锚；下表将精确函数锚与混合职责明确区分。不能将文件首锚当作逐函数等价证明。

## 逐定义搬移

| 函数 | 返工前 | 当前 | ZGC 对应或归属 | 口径 |
|---|---|---|---|---|
| `Request` | `runtime/src/Heap/z/zAbort.hpp:16` | `runtime/src/Heap/z/zAbort.cpp:10` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAbort.cpp:28 ZAbort::abort | 定义对应 |
| `IsRequested` | `runtime/src/Heap/z/zAbort.hpp:18` | `runtime/src/Heap/z/zAbort.inline.hpp:11` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAbort.inline.hpp:29 ZAbort::should_abort | 定义对应 |
| `Poll` | `runtime/src/Heap/z/zAbort.hpp:19` | `runtime/src/Heap/z/zAbort.inline.hpp:15` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zAbort.inline.hpp:29 ZAbort::should_abort | 定义对应 |
| `CollectorResources` | `runtime/src/Heap/z/zDriver.hpp:42` | `runtime/src/Heap/z/zDriver.cpp:623` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDriver.cpp:108 ZDriver::ZDriver | 定义对应 |
| `claim` | `runtime/src/Heap/z/zForwarding.hpp:79` | `runtime/src/Heap/z/zForwarding.cpp:145` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:51 ZForwarding::claim | 定义对应 |
| `mark_done` | `runtime/src/Heap/z/zForwarding.hpp:86` | `runtime/src/Heap/z/zForwarding.cpp:153` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:188 ZForwarding::mark_done | 定义对应 |
| `is_done` | `runtime/src/Heap/z/zForwarding.hpp:91` | `runtime/src/Heap/z/zForwarding.cpp:160` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:192 ZForwarding::is_done | 定义对应 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zForwarding.hpp:117` | `runtime/src/Heap/z/zForwarding.cpp:166` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `release_page` | `runtime/src/Heap/z/zForwarding.hpp:116` | `runtime/src/Heap/z/zForwarding.cpp:165` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:134 ZForwarding::release_page | 定义对应 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zForwarding.hpp:142` | `runtime/src/Heap/z/zForwarding.cpp:192` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `in_place_relocation_claim_page` | `runtime/src/Heap/z/zForwarding.hpp:141` | `runtime/src/Heap/z/zForwarding.cpp:191` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:110 ZForwarding::in_place_relocation_claim_page | 定义对应 |
| `detach_page` | `runtime/src/Heap/z/zForwarding.hpp:157` | `runtime/src/Heap/z/zForwarding.cpp:208` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:171 ZForwarding::detach_page | 定义对应 |
| `nentries` | `runtime/src/Heap/z/zForwarding.hpp:265` | `runtime/src/Heap/z/zForwarding.inline.hpp:54` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:43 ZForwarding::nentries | 定义对应 |
| `alloc` | `runtime/src/Heap/z/zForwarding.hpp:283` | `runtime/src/Heap/z/zForwarding.inline.hpp:74` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:53 ZForwarding::alloc | 定义对应 |
| `start` | `runtime/src/Heap/z/zForwarding.hpp:315` | `runtime/src/Heap/z/zForwarding.inline.hpp:96` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:90 ZForwarding::start | 定义对应 |
| `size` | `runtime/src/Heap/z/zForwarding.hpp:316` | `runtime/src/Heap/z/zForwarding.inline.hpp:100` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:98 ZForwarding::size | 定义对应 |
| `page` | `runtime/src/Heap/z/zForwarding.hpp:318` | `runtime/src/Heap/z/zForwarding.cpp:217` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:183 ZForwarding::page | 定义对应 |
| `index` | `runtime/src/Heap/z/zForwarding.hpp:361` | `runtime/src/Heap/z/zForwarding.inline.hpp:104` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:226 ZForwarding::index | 定义对应 |
| `entries` | `runtime/src/Heap/z/zForwarding.hpp:363` | `runtime/src/Heap/z/zForwarding.inline.hpp:108` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:203 ZForwarding::entries | 定义对应 |
| `at` | `runtime/src/Heap/z/zForwarding.hpp:366` | `runtime/src/Heap/z/zForwarding.inline.hpp:113` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:207 ZForwarding::at | 定义对应 |
| `first` | `runtime/src/Heap/z/zForwarding.hpp:372` | `runtime/src/Heap/z/zForwarding.inline.hpp:121` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:213 ZForwarding::first | 定义对应 |
| `next` | `runtime/src/Heap/z/zForwarding.hpp:379` | `runtime/src/Heap/z/zForwarding.inline.hpp:130` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:220 ZForwarding::next | 定义对应 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zForwarding.hpp:390` | `runtime/src/Heap/z/zForwarding.inline.hpp:141` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `find` | `runtime/src/Heap/z/zForwarding.hpp:388` | `runtime/src/Heap/z/zForwarding.inline.hpp:139` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:230 ZForwarding::find; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:248 ZForwarding::find; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:254 ZForwarding::find; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:258 ZForwarding::find; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:262 ZForwarding::find | 定义对应 |
| `find` | `runtime/src/Heap/z/zForwarding.hpp:432` | `runtime/src/Heap/z/zForwarding.inline.hpp:153` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:230 ZForwarding::find; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:248 ZForwarding::find; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:254 ZForwarding::find; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:258 ZForwarding::find; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:262 ZForwarding::find | 定义对应 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zForwarding.hpp:482` | `runtime/src/Heap/z/zForwarding.inline.hpp:189` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zForwarding.hpp:467` | `runtime/src/Heap/z/zForwarding.inline.hpp:174` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `insert` | `runtime/src/Heap/z/zForwarding.hpp:463` | `runtime/src/Heap/z/zForwarding.inline.hpp:170` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:267 ZForwarding::insert; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:295 ZForwarding::insert; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:302 ZForwarding::insert | 定义对应 |
| `insert` | `runtime/src/Heap/z/zForwarding.hpp:579` | `runtime/src/Heap/z/zForwarding.inline.hpp:216` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:267 ZForwarding::insert; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:295 ZForwarding::insert; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:302 ZForwarding::insert | 定义对应 |
| `relocated_remembered_fields_register` | `runtime/src/Heap/z/zForwarding.hpp:603` | `runtime/src/Heap/z/zForwarding.inline.hpp:223` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:306 ZForwarding::relocated_remembered_fields_register | 定义对应 |
| `relocated_remembered_fields_is_concurrently_scanned` | `runtime/src/Heap/z/zForwarding.hpp:613` | `runtime/src/Heap/z/zForwarding.inline.hpp:235` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:329 ZForwarding::relocated_remembered_fields_is_concurrently_scanned | 定义对应 |
| `relocated_remembered_fields_published_contains` | `runtime/src/Heap/z/zForwarding.hpp:620` | `runtime/src/Heap/z/zForwarding.cpp:222` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:359 ZForwarding::relocated_remembered_fields_published_contains | 定义对应 |
| `relocated_remembered_fields_after_relocate` | `runtime/src/Heap/z/zForwarding.hpp:629` | `runtime/src/Heap/z/zForwarding.cpp:233` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:275 ZForwarding::relocated_remembered_fields_after_relocate | 定义对应 |
| `relocated_remembered_fields_publish` | `runtime/src/Heap/z/zForwarding.hpp:637` | `runtime/src/Heap/z/zForwarding.cpp:243` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:285 ZForwarding::relocated_remembered_fields_publish | 定义对应 |
| `relocated_remembered_fields_notify_concurrent_scan_of` | `runtime/src/Heap/z/zForwarding.hpp:647` | `runtime/src/Heap/z/zForwarding.cpp:255` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:317 ZForwarding::relocated_remembered_fields_notify_concurrent_scan_of | 定义对应 |
| `relocated_remembered_fields_apply_to_published` | `runtime/src/Heap/z/zForwarding.hpp:665` | `runtime/src/Heap/z/zForwarding.inline.hpp:244` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp:334 ZForwarding::relocated_remembered_fields_apply_to_published | 定义对应 |
| `ZForwarding` | `runtime/src/Heap/z/zForwarding.hpp:720` | `runtime/src/Heap/z/zForwarding.inline.hpp:282` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `Get` | `runtime/src/Heap/z/zForwardingTable.hpp:143` | `runtime/src/Heap/z/zForwardingTable.inline.hpp:11` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwardingTable.inline.hpp:43 ZForwardingTable::get | 定义对应 |
| `ResetY2yHandoffTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:126` | `runtime/src/Heap/Collector/Generation.cpp:87` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ReadY2yHandoffTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:138` | `runtime/src/Heap/Collector/Generation.cpp:99` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `NoteY2yBeforeReleaseTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:148` | `runtime/src/Heap/Collector/Generation.cpp:109` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `NoteY2yAfterRootTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:154` | `runtime/src/Heap/Collector/Generation.cpp:115` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `NoteY2yAfterStw2TestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:160` | `runtime/src/Heap/Collector/Generation.cpp:121` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ArmY2yAfterReleaseTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:166` | `runtime/src/Heap/Collector/Generation.cpp:127` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zGeneration.cpp:173` | `runtime/src/Heap/Collector/Generation.cpp:134` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `PublishY2yAfterReleaseTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:172` | `runtime/src/Heap/Collector/Generation.cpp:133` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ArmMarkBeforeMarkEndTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:189` | `runtime/src/Heap/Collector/Generation.cpp:150` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `PublishMarkBeforeMarkEndTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:197` | `runtime/src/Heap/Collector/Generation.cpp:158` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ArmAllocBlackDuringConcurrentTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:216` | `runtime/src/Heap/Collector/Generation.cpp:177` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ArmY2yDuringConcurrentTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:221` | `runtime/src/Heap/Collector/Generation.cpp:182` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `PublishConcurrentYoungProducersTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:226` | `runtime/src/Heap/Collector/Generation.cpp:187` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ArmLeftoverBeforePauseTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:238` | `runtime/src/Heap/Collector/Generation.cpp:199` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `PublishLeftoverBeforePauseTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:244` | `runtime/src/Heap/Collector/Generation.cpp:205` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ResetExportRootPublicationTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:256` | `runtime/src/Heap/Collector/Generation.cpp:217` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ArmExportRootAfterT1TestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:270` | `runtime/src/Heap/Collector/Generation.cpp:231` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `PublishExportRootAfterT1TestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:280` | `runtime/src/Heap/Collector/Generation.cpp:241` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `FlushExportRootAfterT1TestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:296` | `runtime/src/Heap/Collector/Generation.cpp:257` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zGeneration.cpp:313` | `runtime/src/Heap/Collector/Generation.cpp:274` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `NoteExportRootPublicationAtT2TestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:307` | `runtime/src/Heap/Collector/Generation.cpp:268` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ReadExportRootPublicationTestReceipt` | `runtime/src/Heap/z/zGeneration.cpp:324` | `runtime/src/Heap/Collector/Generation.cpp:285` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `HeapIterator` | `runtime/src/Heap/z/zHeapIterator.hpp:20` | `runtime/src/Heap/z/zHeapIterator.cpp:76` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zHeapIterator.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `claim_level_size` | `runtime/src/Heap/z/zIndexDistributor.hpp:23` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp:19` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zIndexDistributor.inline.hpp:120 ZIndexDistributorClaimTree::claim_level_size | 定义对应 |
| `claim_level_end_index` | `runtime/src/Heap/z/zIndexDistributor.hpp:28` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp:24` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zIndexDistributor.inline.hpp:129 ZIndexDistributorClaimTree::claim_level_end_index | 定义对应 |
| `~ZIndexDistributorClaimTree` | `runtime/src/Heap/z/zIndexDistributor.hpp:48` | `runtime/src/Heap/z/zIndexDistributor.inline.hpp:44` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zIndexDistributor.inline.hpp:289 ZIndexDistributorClaimTree::~ZIndexDistributorClaimTree | 定义对应 |
| `AddLiveCounts` | `runtime/src/Heap/z/zLiveMap.hpp:96` | `runtime/src/Heap/z/zLiveMap.inline.hpp:30` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:117 ZLiveMap::inc_live | 定义对应 |
| `Reset` | `runtime/src/Heap/z/zLiveMap.hpp:118` | `runtime/src/Heap/z/zLiveMap.inline.hpp:106` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:37 ZLiveMap::reset | 定义对应 |
| `IsSegmentLive` | `runtime/src/Heap/z/zLiveMap.hpp:135` | `runtime/src/Heap/z/zLiveMap.inline.hpp:12` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:69 ZLiveMap::is_segment_live | 定义对应 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zLiveMap.hpp:155` | `runtime/src/Heap/z/zLiveMap.cpp:25` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `EnsureSegmentLive` | `runtime/src/Heap/z/zLiveMap.hpp:141` | `runtime/src/Heap/z/zLiveMap.cpp:11` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.cpp:109 ZLiveMap::reset_segment | 定义对应 |
| `GetLiveObjects` | `runtime/src/Heap/z/zLiveMap.hpp:168` | `runtime/src/Heap/z/zLiveMap.inline.hpp:37` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:45 ZLiveMap::live_objects | 定义对应 |
| `MarkBits` | `runtime/src/Heap/z/zLiveMap.hpp:171` | `runtime/src/Heap/z/zLiveMap.inline.hpp:42` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:100 ZLiveMap::set | 定义对应 |
| `MarkBits` | `runtime/src/Heap/z/zLiveMap.hpp:187` | `runtime/src/Heap/z/zLiveMap.inline.hpp:60` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:100 ZLiveMap::set | 定义对应 |
| `MarkFinalizableBits` | `runtime/src/Heap/z/zLiveMap.hpp:197` | `runtime/src/Heap/z/zLiveMap.inline.hpp:72` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:100 ZLiveMap::set | 定义对应 |
| `IsMarked` | `runtime/src/Heap/z/zLiveMap.hpp:210` | `runtime/src/Heap/z/zLiveMap.inline.hpp:19` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:41 ZLiveMap::is_marked | 定义对应 |
| `IsLive` | `runtime/src/Heap/z/zLiveMap.hpp:219` | `runtime/src/Heap/z/zLiveMap.inline.hpp:87` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:93 ZLiveMap::get | 定义对应 |
| `IsFinalizable` | `runtime/src/Heap/z/zLiveMap.hpp:227` | `runtime/src/Heap/z/zLiveMap.inline.hpp:97` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:93 ZLiveMap::get | 定义对应 |
| `GetLiveBytes` | `runtime/src/Heap/z/zLiveMap.hpp:232` | `runtime/src/Heap/z/zLiveMap.inline.hpp:101` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zLiveMap.inline.hpp:49 ZLiveMap::live_bytes | 定义对应 |
| `MinSizeWatermark` | `runtime/src/Heap/z/zMappedCache.hpp:31` | `runtime/src/Heap/z/zMappedCache.cpp:142` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMappedCache.cpp:692 ZMappedCache::min_size_watermark | 定义对应 |
| `ResetMinSizeWatermark` | `runtime/src/Heap/z/zMappedCache.hpp:32` | `runtime/src/Heap/z/zMappedCache.cpp:146` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMappedCache.cpp:688 ZMappedCache::reset_min_size_watermark | 定义对应 |
| `ResetYoungWeakClosureTestReceipt` | `runtime/src/Heap/z/zMark.cpp:73` | `runtime/src/Heap/Collector/Mark.cpp:65` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `NoteYoungWeakClosureDiscovery` | `runtime/src/Heap/z/zMark.cpp:80` | `runtime/src/Heap/Collector/Mark.cpp:72` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ReadYoungWeakClosureTestReceipt` | `runtime/src/Heap/z/zMark.cpp:95` | `runtime/src/Heap/Collector/Mark.cpp:87` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `TracingCollector` | `runtime/src/Heap/z/zMark.hpp:258` | `runtime/src/Heap/z/zMark.cpp:2244` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `DumpBeforeGC` | `runtime/src/Heap/z/zMark.hpp:312` | `runtime/src/Heap/Collector/TracingCollector.cpp:312` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `DumpAfterGC` | `runtime/src/Heap/z/zMark.hpp:324` | `runtime/src/Heap/Collector/TracingCollector.cpp:324` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `MarkObject` | `runtime/src/Heap/z/zMark.hpp:381` | `runtime/src/Heap/z/zMark.inline.hpp:12` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.inline.hpp:49 ZMark::mark_object | 定义对应 |
| `FollowPartialArray` | `runtime/src/Heap/z/zMark.hpp:413` | `runtime/src/Heap/z/zMark.cpp:2249` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.cpp:265 ZMark::follow_partial_array | 定义对应 |
| `StripeId` | `runtime/src/Heap/z/zMarkContext.hpp:19` | `runtime/src/Heap/z/zMarkContext.inline.hpp:18` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkContext.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `NStripes` | `runtime/src/Heap/z/zMarkContext.hpp:20` | `runtime/src/Heap/z/zMarkContext.inline.hpp:22` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkContext.inline.hpp:53 ZMarkContext::nstripes | 定义对应 |
| `SetNStripes` | `runtime/src/Heap/z/zMarkContext.hpp:21` | `runtime/src/Heap/z/zMarkContext.inline.hpp:26` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkContext.inline.hpp:57 ZMarkContext::set_nstripes | 定义对应 |
| `SetStripeId` | `runtime/src/Heap/z/zMarkContext.hpp:23` | `runtime/src/Heap/z/zMarkContext.inline.hpp:31` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkContext.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `Stacks` | `runtime/src/Heap/z/zMarkContext.hpp:27` | `runtime/src/Heap/z/zMarkContext.inline.hpp:38` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkContext.inline.hpp:49 ZMarkContext::stacks | 定义对应 |
| `Cache` | `runtime/src/Heap/z/zMarkContext.hpp:28` | `runtime/src/Heap/z/zMarkContext.inline.hpp:42` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkContext.inline.hpp:37 ZMarkContext::cache | 定义对应 |
| `IsEmpty` | `runtime/src/Heap/z/zMarkStack.hpp:39` | `runtime/src/Heap/z/zMarkStack.inline.hpp:11` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp:33 ZMarkStack::is_empty; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp:55 ZMarkStripe::is_empty | 定义对应 |
| `IsFull` | `runtime/src/Heap/z/zMarkStack.hpp:40` | `runtime/src/Heap/z/zMarkStack.inline.hpp:12` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp:37 ZMarkStack::is_full | 定义对应 |
| `Size` | `runtime/src/Heap/z/zMarkStack.hpp:41` | `runtime/src/Heap/z/zMarkStack.inline.hpp:13` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `Capacity` | `runtime/src/Heap/z/zMarkStack.hpp:42` | `runtime/src/Heap/z/zMarkStack.inline.hpp:14` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `MarkStripeStackListNode` | `runtime/src/Heap/z/zMarkStack.hpp:63` | `runtime/src/Heap/z/zMarkStack.cpp:298` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:56 ZMarkStackListNode::ZMarkStackListNode | 定义对应 |
| `Stack` | `runtime/src/Heap/z/zMarkStack.hpp:65` | `runtime/src/Heap/z/zMarkStack.cpp:300` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:58 ZMarkStackListNode::stack | 定义对应 |
| `Next` | `runtime/src/Heap/z/zMarkStack.hpp:66` | `runtime/src/Heap/z/zMarkStack.cpp:302` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:62 ZMarkStackListNode::next | 定义对应 |
| `SetNext` | `runtime/src/Heap/z/zMarkStack.hpp:67` | `runtime/src/Heap/z/zMarkStack.cpp:304` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:66 ZMarkStackListNode::set_next | 定义对应 |
| `IsEmpty` | `runtime/src/Heap/z/zMarkStack.hpp:85` | `runtime/src/Heap/z/zMarkStack.cpp:306` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:236 ZMarkStripeSet::is_empty; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:288 ZMarkThreadLocalStacks::is_empty; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:74 ZMarkStackList::is_empty | 定义对应 |
| `IsEmpty` | `runtime/src/Heap/z/zMarkStack.hpp:99` | `runtime/src/Heap/z/zMarkStack.inline.hpp:15` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp:33 ZMarkStack::is_empty; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp:55 ZMarkStripe::is_empty | 定义对应 |
| `Population` | `runtime/src/Heap/z/zMarkStack.hpp:100` | `runtime/src/Heap/z/zMarkStack.cpp:308` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:193 ZMarkStripe::population | 定义对应 |
| `NStripes` | `runtime/src/Heap/z/zMarkStack.hpp:118` | `runtime/src/Heap/z/zMarkStack.cpp:310` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:232 ZMarkStripeSet::nstripes | 定义对应 |
| `Next` | `runtime/src/Heap/z/zMarkStack.hpp:131` | `runtime/src/Heap/z/zMarkStack.inline.hpp:16` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `Next` | `runtime/src/Heap/z/zMarkStack.hpp:132` | `runtime/src/Heap/z/zMarkStack.inline.hpp:17` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `At` | `runtime/src/Heap/z/zMarkStack.hpp:133` | `runtime/src/Heap/z/zMarkStack.inline.hpp:18` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `At` | `runtime/src/Heap/z/zMarkStack.hpp:134` | `runtime/src/Heap/z/zMarkStack.inline.hpp:19` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.inline.hpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `MarkStripeStack` | `runtime/src/Heap/z/zMarkStack.inline.hpp:13` | `runtime/src/Heap/z/zMarkStack.cpp:84` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:52 ZMarkStack::ZMarkStack | 定义对应 |
| `SetStorageObserver` | `runtime/src/Heap/z/zMarkStack.inline.hpp:17` | `runtime/src/Heap/z/zMarkStack.cpp:88` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `Length` | `runtime/src/Heap/z/zMarkStack.inline.hpp:35` | `runtime/src/Heap/z/zMarkStack.cpp:94` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:169 ZMarkStackList::length | 定义对应 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zMarkStack.inline.hpp:47` | `runtime/src/Heap/z/zMarkStack.cpp:106` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `Push` | `runtime/src/Heap/z/zMarkStack.inline.hpp:41` | `runtime/src/Heap/z/zMarkStack.cpp:100` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:78 ZMarkStackList::push | 定义对应 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zMarkStack.inline.hpp:61` | `runtime/src/Heap/z/zMarkStack.cpp:120` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `Pop` | `runtime/src/Heap/z/zMarkStack.inline.hpp:58` | `runtime/src/Heap/z/zMarkStack.cpp:117` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:105 ZMarkStackList::pop | 定义对应 |
| `StealStack` | `runtime/src/Heap/z/zMarkStack.inline.hpp:103` | `runtime/src/Heap/z/zMarkStack.cpp:150` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:183 ZMarkStripe::steal_stack | 定义对应 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zMarkStack.inline.hpp:113` | `runtime/src/Heap/z/zMarkStack.cpp:160` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `MarkStripeSet` | `runtime/src/Heap/z/zMarkStack.inline.hpp:110` | `runtime/src/Heap/z/zMarkStack.cpp:157` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:199 ZMarkStripeSet::ZMarkStripeSet | 定义对应 |
| `SetNStripes` | `runtime/src/Heap/z/zMarkStack.inline.hpp:120` | `runtime/src/Heap/z/zMarkStack.cpp:167` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:201 ZMarkStripeSet::set_nstripes | 定义对应 |
| `TrySetNStripes` | `runtime/src/Heap/z/zMarkStack.inline.hpp:127` | `runtime/src/Heap/z/zMarkStack.cpp:174` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:213 ZMarkStripeSet::try_set_nstripes | 定义对应 |
| `CalculateNStripes` | `runtime/src/Heap/z/zMarkStack.inline.hpp:135` | `runtime/src/Heap/z/zMarkStack.cpp:182` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `IsCrowded` | `runtime/src/Heap/z/zMarkStack.inline.hpp:146` | `runtime/src/Heap/z/zMarkStack.cpp:193` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:246 ZMarkStripeSet::is_crowded | 定义对应 |
| `IsEmpty` | `runtime/src/Heap/z/zMarkStack.inline.hpp:159` | `runtime/src/Heap/z/zMarkStack.cpp:206` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:236 ZMarkStripeSet::is_empty; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:288 ZMarkThreadLocalStacks::is_empty; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:74 ZMarkStackList::is_empty | 定义对应 |
| `Population` | `runtime/src/Heap/z/zMarkStack.inline.hpp:169` | `runtime/src/Heap/z/zMarkStack.cpp:216` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:193 ZMarkStripe::population | 定义对应 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zMarkStack.inline.hpp:179` | `runtime/src/Heap/z/zMarkStack.cpp:226` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `FirstNonEmptyStripe` | `runtime/src/Heap/z/zMarkStack.inline.hpp:178` | `runtime/src/Heap/z/zMarkStack.cpp:225` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `StripeForWorker` | `runtime/src/Heap/z/zMarkStack.inline.hpp:193` | `runtime/src/Heap/z/zMarkStack.cpp:235` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:260 ZMarkStripeSet::stripe_for_worker | 定义对应 |
| `MarkThreadLocalStacks` | `runtime/src/Heap/z/zMarkStack.inline.hpp:208` | `runtime/src/Heap/z/zMarkStack.cpp:250` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:282 ZMarkThreadLocalStacks::ZMarkThreadLocalStacks | 定义对应 |
| `~MarkThreadLocalStacks` | `runtime/src/Heap/z/zMarkStack.inline.hpp:211` | `runtime/src/Heap/z/zMarkStack.cpp:253` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `IsEmpty` | `runtime/src/Heap/z/zMarkStack.inline.hpp:218` | `runtime/src/Heap/z/zMarkStack.cpp:260` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:236 ZMarkStripeSet::is_empty; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:288 ZMarkThreadLocalStacks::is_empty; /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:74 ZMarkStackList::is_empty | 定义对应 |
| `Population` | `runtime/src/Heap/z/zMarkStack.inline.hpp:228` | `runtime/src/Heap/z/zMarkStack.cpp:270` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:193 ZMarkStripe::population | 定义对应 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zMarkStack.inline.hpp:298` | `runtime/src/Heap/z/zMarkStack.cpp:286` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `Flush` | `runtime/src/Heap/z/zMarkStack.inline.hpp:296` | `runtime/src/Heap/z/zMarkStack.cpp:284` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMarkStack.cpp:300 ZMarkThreadLocalStacks::flush | 定义对应 |
| `GcMetronome` | `runtime/src/Heap/z/zMetronome.hpp:13` | `runtime/src/Heap/z/zMetronome.cpp:11` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMetronome.cpp:34 ZMetronome::ZMetronome | 定义对应 |
| `Count` | `runtime/src/Heap/z/zNUMA.hpp:18` | `runtime/src/Heap/z/zNUMA.inline.hpp:11` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zNUMA.inline.hpp:41 ZNUMA::count | 定义对应 |
| `RegionManager` | `runtime/src/Heap/z/zObjectAllocator.hpp:31` | `runtime/src/Heap/z/zObjectAllocator.cpp:481` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zObjectAllocator.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zPage.cpp:75` | `runtime/src/Heap/Allocator/zPage.cpp:138` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `SetGhostLookupTestHook` | `runtime/src/Heap/z/zPage.cpp:80` | `runtime/src/Heap/Allocator/zPage.cpp:143` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `GhostLookupTestHookCalls` | `runtime/src/Heap/z/zPage.cpp:86` | `runtime/src/Heap/Allocator/zPage.cpp:149` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `RunGhostLookupTestHook` | `runtime/src/Heap/z/zPage.cpp:91` | `runtime/src/Heap/Allocator/zPage.cpp:154` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `NoteEnrolPhase` | `runtime/src/Heap/z/zPage.cpp:323` | `runtime/src/Heap/Allocator/zPage.cpp:60` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `RegionInfo` | `runtime/src/Heap/z/zPage.hpp:174` | `runtime/src/Heap/z/zPage.cpp:321` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.cpp:59 ZPage::ZPage | 定义对应 |
| `GetOwnerGeneration` | `runtime/src/Heap/z/zPage.hpp:382` | `runtime/src/Heap/z/zPage.inline.hpp:2585` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:119 ZPage::generation_id | 定义对应 |
| `generation_id` | `runtime/src/Heap/z/zPage.hpp:754` | `runtime/src/Heap/z/zPage.inline.hpp:2578` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:119 ZPage::generation_id | 定义对应 |
| `IsYoungRegion` | `runtime/src/Heap/z/zPage.hpp:970` | `runtime/src/Heap/z/zPage.inline.hpp:2592` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:123 ZPage::is_young | 定义对应 |
| `GetRegionStart` | `runtime/src/Heap/z/zPage.hpp:992` | `runtime/src/Heap/z/zPage.inline.hpp:2598` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:131 ZPage::start | 定义对应 |
| `GetRegionEnd` | `runtime/src/Heap/z/zPage.hpp:994` | `runtime/src/Heap/z/zPage.inline.hpp:2602` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:135 ZPage::end | 定义对应 |
| `GetRegionAllocPtr` | `runtime/src/Heap/z/zPage.hpp:998` | `runtime/src/Heap/z/zPage.inline.hpp:2606` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:143 ZPage::top | 定义对应 |
| `IsSmallRegion` | `runtime/src/Heap/z/zPage.hpp:1033` | `runtime/src/Heap/z/zPage.inline.hpp:2610` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:107 ZPage::is_small | 定义对应 |
| `IsLargeRegion` | `runtime/src/Heap/z/zPage.hpp:1035` | `runtime/src/Heap/z/zPage.inline.hpp:2614` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zPage.inline.hpp:115 ZPage::is_large | 定义对应 |
| `RelocateObserve` | `runtime/src/Heap/z/zPage.inline.hpp:15` | `runtime/src/Heap/Allocator/RegionInfo.h:12` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `EnrolBeforeFlip` | `runtime/src/Heap/z/zPage.inline.hpp:82` | `runtime/src/Heap/Allocator/RegionInfo.h:31` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `EnrolAfterFlip` | `runtime/src/Heap/z/zPage.inline.hpp:88` | `runtime/src/Heap/Allocator/RegionInfo.h:37` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `GetLiveInfo0ForProbe` | `runtime/src/Heap/z/zPage.inline.hpp:139` | `runtime/src/Heap/Allocator/RegionInfo.h:43` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zPage.inline.hpp:777` | `runtime/src/Heap/Allocator/RegionInfo.h:50` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `PageOwnerVerifyCountOnly` | `runtime/src/Heap/z/zPage.inline.hpp:776` | `runtime/src/Heap/Allocator/RegionInfo.h:49` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `PageOwnerMismatchAttempts` | `runtime/src/Heap/z/zPage.inline.hpp:785` | `runtime/src/Heap/Allocator/RegionInfo.h:58` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `PageOwnerMismatchFirstPaints` | `runtime/src/Heap/z/zPage.inline.hpp:791` | `runtime/src/Heap/Allocator/RegionInfo.h:64` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ReportPageOwnerVerifyCounts` | `runtime/src/Heap/z/zPage.inline.hpp:797` | `runtime/src/Heap/Allocator/RegionInfo.h:70` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `atexit` | `runtime/src/Heap/z/zPage.inline.hpp:808` | `runtime/src/Heap/Allocator/RegionInfo.h:81` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zPage.inline.hpp:807` | `runtime/src/Heap/Allocator/RegionInfo.h:80` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `EnsurePageOwnerVerifyAtexit` | `runtime/src/Heap/z/zPage.inline.hpp:806` | `runtime/src/Heap/Allocator/RegionInfo.h:79` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `NotePageOwnerFirstPaint` | `runtime/src/Heap/z/zPage.inline.hpp:840` | `runtime/src/Heap/Allocator/RegionInfo.h:89` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ReportMarkEpochCounts` | `runtime/src/Heap/z/zPage.inline.hpp:1047` | `runtime/src/Heap/Allocator/RegionInfo.h:96` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `atexit` | `runtime/src/Heap/z/zPage.inline.hpp:1058` | `runtime/src/Heap/Allocator/RegionInfo.h:107` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `EnsureMarkEpochAtexit` | `runtime/src/Heap/z/zPage.inline.hpp:1055` | `runtime/src/Heap/Allocator/RegionInfo.h:104` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `GetRouteForProbe` | `runtime/src/Heap/z/zPage.inline.hpp:1721` | `runtime/src/Heap/Allocator/RegionInfo.h:112` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ReportTypeInfoInHeap` | `runtime/src/Heap/z/zPage.inline.hpp:2535` | `runtime/src/Heap/Allocator/RegionInfo.h:122` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ReportInvalidObjectSize` | `runtime/src/Heap/z/zPage.inline.hpp:2553` | `runtime/src/Heap/Allocator/RegionInfo.h:140` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `Enqueue` | `runtime/src/Heap/z/zPageAllocator.cpp:71` | `runtime/src/Heap/Allocator/zPageAllocator.cpp:67` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `Dequeue` | `runtime/src/Heap/z/zPageAllocator.cpp:84` | `runtime/src/Heap/Allocator/zPageAllocator.cpp:80` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `Report` | `runtime/src/Heap/z/zPageAllocator.cpp:95` | `runtime/src/Heap/Allocator/zPageAllocator.cpp:91` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `atexit` | `runtime/src/Heap/z/zRelocate.cpp:131` | `runtime/src/Heap/Collector/zRelocate.cpp:26` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `NoteFwdToGateRefuse` | `runtime/src/Heap/z/zRelocate.cpp:128` | `runtime/src/Heap/Collector/zRelocate.cpp:23` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ResetRemapYoungRootsTestReceipt` | `runtime/src/Heap/z/zRelocate.cpp:157` | `runtime/src/Heap/Collector/zRelocate.cpp:172` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ReadRemapYoungRootsTestReceipt` | `runtime/src/Heap/z/zRelocate.cpp:168` | `runtime/src/Heap/Collector/zRelocate.cpp:183` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `NoteRemapYoungRootsTestReceipt` | `runtime/src/Heap/z/zRelocate.cpp:180` | `runtime/src/Heap/Collector/zRelocate.cpp:195` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `MRT_SetRemapWindowTestHook` | `runtime/src/Heap/z/zRelocate.cpp:205` | `runtime/src/Heap/Collector/zRelocate.cpp:338` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `RunRemapWindowTestHook` | `runtime/src/Heap/z/zRelocate.cpp:209` | `runtime/src/Heap/Collector/zRelocate.cpp:342` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ResetRemsetFilterTestReceipt` | `runtime/src/Heap/z/zRemembered.cpp:75` | `runtime/src/Heap/Collector/Remembered.cpp:75` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ReadRemsetFilterTestReceipt` | `runtime/src/Heap/z/zRemembered.cpp:90` | `runtime/src/Heap/Collector/Remembered.cpp:90` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `NoteRemsetFilterTestReceipt` | `runtime/src/Heap/z/zRemembered.cpp:105` | `runtime/src/Heap/Collector/Remembered.cpp:105` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `IsInitialized` | `runtime/src/Heap/z/zRememberedSet.hpp:48` | `runtime/src/Heap/z/zRememberedSet.cpp:611` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zRememberedSet.cpp:46 ZRememberedSet::is_initialized | 定义对应 |
| `StackMapInvalidReasonName` | `runtime/src/Heap/z/zRootsIterator.cpp:40` | `runtime/src/Heap/Collector/TracingCollector.cpp:216` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ResetSkippedStackMapCounts` | `runtime/src/Heap/z/zRootsIterator.cpp:55` | `runtime/src/Heap/Collector/TracingCollector.cpp:231` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `RecordRootMapMiss` | `runtime/src/Heap/z/zRootsIterator.cpp:63` | `runtime/src/Heap/Collector/TracingCollector.cpp:239` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `RecordSkippedStackMap` | `runtime/src/Heap/z/zRootsIterator.cpp:78` | `runtime/src/Heap/Collector/TracingCollector.cpp:256` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `ReportSkippedStackMapCounts` | `runtime/src/Heap/z/zRootsIterator.cpp:111` | `runtime/src/Heap/Collector/TracingCollector.cpp:289` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `CurrentThreadRootMapMissCount` | `runtime/src/Heap/z/zRootsIterator.cpp:125` | `runtime/src/Heap/Collector/TracingCollector.cpp:301` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `<lambda-or-initializer>` | `runtime/src/Heap/z/zStackWatermark.cpp:16` | `runtime/src/UnwindStack/StackWatermark.cpp:15` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `VerifyEnabled` | `runtime/src/Heap/z/zStackWatermark.cpp:15` | `runtime/src/UnwindStack/StackWatermark.cpp:14` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `StackWatermark` | `runtime/src/Heap/z/zStackWatermark.hpp:58` | `runtime/src/Heap/z/zStackWatermark.cpp:18` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStackWatermark.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `IsDone` | `runtime/src/Heap/z/zStackWatermark.hpp:292` | `runtime/src/Heap/z/zStackWatermark.cpp:22` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStackWatermark.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `IsDone` | `runtime/src/Heap/z/zStackWatermark.hpp:294` | `runtime/src/Heap/z/zStackWatermark.cpp:27` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStackWatermark.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `Add` | `runtime/src/Heap/z/zStat.hpp:87` | `runtime/src/Heap/z/zStat.cpp:669` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:71 ZStatSamplerData::add | 定义对应 |
| `Add` | `runtime/src/Heap/z/zStat.hpp:130` | `runtime/src/Heap/z/zStat.cpp:678` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:174 ZStatSamplerHistory::add | 定义对应 |
| `Group` | `runtime/src/Heap/z/zStat.hpp:160` | `runtime/src/Heap/z/zStat.cpp:686` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:371 ZStatValue::group | 定义对应 |
| `Name` | `runtime/src/Heap/z/zStat.hpp:161` | `runtime/src/Heap/z/zStat.cpp:690` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:375 ZStatValue::name | 定义对应 |
| `Id` | `runtime/src/Heap/z/zStat.hpp:162` | `runtime/src/Heap/z/zStat.cpp:694` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:379 ZStatValue::id | 定义对应 |
| `ZStatPhase` | `runtime/src/Heap/z/zStat.hpp:228` | `runtime/src/Heap/z/zStat.cpp:701` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:598 ZStatPhase::ZStatPhase | 定义对应 |
| `Name` | `runtime/src/Heap/z/zStat.hpp:229` | `runtime/src/Heap/z/zStat.cpp:705` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:626 ZStatPhase::name | 定义对应 |
| `RegisterEnd` | `runtime/src/Heap/z/zStat.hpp:230` | `runtime/src/Heap/z/zStat.cpp:709` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:862 ZStatCriticalPhase::register_end | 定义对应 |
| `ZStatCriticalPhase` | `runtime/src/Heap/z/zStat.hpp:241` | `runtime/src/Heap/z/zStat.cpp:714` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `RegisterEnd` | `runtime/src/Heap/z/zStat.hpp:243` | `runtime/src/Heap/z/zStat.cpp:718` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp:862 ZStatCriticalPhase::register_end | 定义对应 |
| `ZStatHeap` | `runtime/src/Heap/z/zStat.hpp:325` | `runtime/src/Heap/z/zStat.cpp:725` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStat.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |
| `IsEmpty` | `runtime/src/Heap/z/zStoreBarrierBuffer.hpp:54` | `runtime/src/Heap/z/zStoreBarrierBuffer.cpp:149` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStoreBarrierBuffer.cpp:68 ZStoreBarrierBuffer::is_empty | 定义对应 |
| `Current` | `runtime/src/Heap/z/zStoreBarrierBuffer.hpp:56` | `runtime/src/Heap/z/zStoreBarrierBuffer.inline.hpp:37` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zStoreBarrierBuffer.inline.hpp:33 ZStoreBarrierBuffer::current | 定义对应 |
| `NoteAllocIntoCSet` | `runtime/src/Heap/z/zThreadLocalAllocBuffer.cpp:49` | `runtime/src/Heap/Allocator/RegionSpace.cpp:95` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `AllocIntoCSetCount` | `runtime/src/Heap/z/zThreadLocalAllocBuffer.cpp:57` | `runtime/src/Heap/Allocator/RegionSpace.cpp:101` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `AllocIntoCSetRetiredCount` | `runtime/src/Heap/z/zThreadLocalAllocBuffer.cpp:62` | `runtime/src/Heap/Allocator/RegionSpace.cpp:106` | ZGC 无独立对应；#228 / A06 原地保留 | 独立自有诊断/观测 |
| `Uncommitter` | `runtime/src/Heap/z/zUncommitter.hpp:24` | `runtime/src/Heap/z/zUncommitter.cpp:262` | /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zUncommitter.cpp | 混合职责/本地辅助：文件职责对应，不声称同名函数等价 |

## 类型与模板例外（advisor 明确裁定）

| 我方 | ZGC | 保留理由与归属 |
|---|---|---|
| zStat.hpp 的 ZStatSamplerData / ZStatSamplerHistoryInterval / ZStatSamplerHistory | zStat.cpp:61、91、156 | 公开类型且 test_zstat.cpp 直接构造；保持可见性与模板实例化，归 A12b（ZStat/serviceability 包）与测试同批改 |
| zForwarding.hpp 的 ZForwardingLife::retain_page | zForwarding.cpp:86 | 我方模板等待适配；不隐藏模板定义，归 D03b #482 |

裁定原文：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_484_implement_r5653671464-20260913T140214Z.md`。引用行号均按当前参考实读，裁定中的旧行号不直接抄作新锚。

## 独立诊断原地保留清单

| 原文件 | 本轮退回内容 | 后续包 |
|---|---|---|
| `runtime/src/Heap/Allocator/RegionInfo.h` | <lambda-or-initializer>, EnrolAfterFlip, EnrolBeforeFlip, EnsureMarkEpochAtexit, EnsurePageOwnerVerifyAtexit, GetLiveInfo0ForProbe, GetRouteForProbe, NotePageOwnerFirstPaint, PageOwnerMismatchAttempts, PageOwnerMismatchFirstPaints, PageOwnerVerifyCountOnly, RelocateObserve, ReportInvalidObjectSize, ReportMarkEpochCounts, ReportPageOwnerVerifyCounts, ReportTypeInfoInHeap, atexit | #228 / A06；机制相关测试观测随所属机制包 |
| `runtime/src/Heap/Allocator/RegionSpace.cpp` | AllocIntoCSetCount, AllocIntoCSetRetiredCount, NoteAllocIntoCSet | #228 / A06；机制相关测试观测随所属机制包 |
| `runtime/src/Heap/Allocator/zPage.cpp` | <lambda-or-initializer>, GhostLookupTestHookCalls, NoteEnrolPhase, RunGhostLookupTestHook, SetGhostLookupTestHook | #228 / A06；机制相关测试观测随所属机制包 |
| `runtime/src/Heap/Allocator/zPageAllocator.cpp` | Dequeue, Enqueue, Report | #228 / A06；机制相关测试观测随所属机制包 |
| `runtime/src/Heap/Collector/Generation.cpp` | <lambda-or-initializer>, ArmAllocBlackDuringConcurrentTestReceipt, ArmExportRootAfterT1TestReceipt, ArmLeftoverBeforePauseTestReceipt, ArmMarkBeforeMarkEndTestReceipt, ArmY2yAfterReleaseTestReceipt, ArmY2yDuringConcurrentTestReceipt, FlushExportRootAfterT1TestReceipt, NoteExportRootPublicationAtT2TestReceipt, NoteY2yAfterRootTestReceipt, NoteY2yAfterStw2TestReceipt, NoteY2yBeforeReleaseTestReceipt, PublishConcurrentYoungProducersTestReceipt, PublishExportRootAfterT1TestReceipt, PublishLeftoverBeforePauseTestReceipt, PublishMarkBeforeMarkEndTestReceipt, PublishY2yAfterReleaseTestReceipt, ReadExportRootPublicationTestReceipt, ReadY2yHandoffTestReceipt, ResetExportRootPublicationTestReceipt, ResetY2yHandoffTestReceipt | #228 / A06；机制相关测试观测随所属机制包 |
| `runtime/src/Heap/Collector/Mark.cpp` | NoteYoungWeakClosureDiscovery, ReadYoungWeakClosureTestReceipt, ResetYoungWeakClosureTestReceipt | #228 / A06；机制相关测试观测随所属机制包 |
| `runtime/src/Heap/Collector/Remembered.cpp` | NoteRemsetFilterTestReceipt, ReadRemsetFilterTestReceipt, ResetRemsetFilterTestReceipt | #228 / A06；机制相关测试观测随所属机制包 |
| `runtime/src/Heap/Collector/TracingCollector.cpp` | CurrentThreadRootMapMissCount, DumpAfterGC, DumpBeforeGC, RecordRootMapMiss, RecordSkippedStackMap, ReportSkippedStackMapCounts, ResetSkippedStackMapCounts, StackMapInvalidReasonName | #228 / A06；机制相关测试观测随所属机制包 |
| `runtime/src/Heap/Collector/zRelocate.cpp` | MRT_SetRemapWindowTestHook, NoteFwdToGateRefuse, NoteRemapYoungRootsTestReceipt, ReadRemapYoungRootsTestReceipt, ResetRemapYoungRootsTestReceipt, RunRemapWindowTestHook, atexit | #228 / A06；机制相关测试观测随所属机制包 |
| `runtime/src/UnwindStack/StackWatermark.cpp` | <lambda-or-initializer>, VerifyEnabled | #228 / A06；机制相关测试观测随所属机制包 |

还原的 RegionManager.h 只声明 RecentFullAccounting，zPageAllocator.cpp 保留其计数与报告。混合函数内部的既有诊断调用保持，不能为拆诊断改函数体。

## 测试同批搬移

沿用上一轮 `docs/gc_z_layout.md` 的 7 项测试路径对应；本轮不增删/改名测试、不开门、不改判据或豁免。统计测试的类型可见性按 advisor 保留。

## 检查口径

函数体多重集与调用锚使用同一冻结基线及同一词法尺，并附受控单函数改变的阳性对照；不能推出运行时行为或性能结论。原按名字 grep 相等的要求已由 advisor 收紧为调用锚相等＋声明差分＋函数体守恒＋全局强定义保持。
