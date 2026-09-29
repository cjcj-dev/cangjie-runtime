待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`
坐标基于 d1f697a053fec85a657286ce5d1c4aeb8fba09b7。锁迁移前生产/消费表；本表只限定同步原语，其他机制归对应条目。

| producer | consumer | ZGC 锚 | 修改点 |
|---|---|---|---|
| zForwarding.cpp:119 release_page | :137 detach_page / :156 in_place_relocation_claim_page | zForwarding.cpp:134-185; zForwarding.hpp:67 | _ref_lock 合并条件变量，保留循环读 _ref_count |
| zRelocate.cpp:384 share_target_page | :367 alloc_and_retire_target_page | zRelocate.cpp:503-570 | lock 合并 changed，循环读 inPlace |
| zRelocate.cpp:626 leave / :745 desynchronize | :637 add_and_wait / :702 synchronize_poll / :735 synchronize | zRelocate.hpp:41; zRelocate.cpp:111-310 | lock 合并 attention，等待者重读原条件 |
| zMarkTerminate.inline.hpp:21 Leave / :63 Wake | :40 TryTerminate | zMarkTerminate.hpp:39; zMarkTerminate.inline.hpp:49-110 | mutex 合并 condition |
| zUncommitter.cpp:139 terminate | :146 WaitUntil | zUncommitter.hpp:37; zUncommitter.cpp:58-87,171-175 | lock 合并 condition，毫秒等待 |
| zDirector.cpp:78 notify_reevaluate / :670 terminate | :85 wait_for_tick | zDirector.hpp:35; zDirector.cpp:843-861 | monitor 合并 condition，ZGC 单次定时等待 |
| zWorkers.cpp:63 起 resize 状态写 | :99 run / zDirector.cpp:621 | zWorkers.hpp:42; zWorkers.cpp:108-123 | _resize_lock -> ZLock，所有持锁点 -> ZLocker |
| zRelocationSet.cpp:199 起 promotion 状态写 | :209,:217 | zRelocationSet.hpp:47 | _promotion_lock -> ZLock |
| zPageAllocator.cpp:675 起认领与释放 | 同文件 :691-886 / zUncommitter.cpp:167-294 | zPageAllocator.hpp:153 | pageAllocatorMutex -> ZLock；cacheMutex 不迁 |
| zStoreBarrierBuffer.cpp:77 install_base_pointers | 同一函数消费 base pointers | zStoreBarrierBuffer.hpp:56; zStoreBarrierBuffer.cpp:105 | basePointerLock -> ZLock |
| zStat.cpp:157-240 Workers / :246-295 Cycle / :784-897 Heap | 同组持锁采样读 | zStat.hpp:424,459,598 | 三成员 -> ZLock |

HeapIterator 当前已用 ZLock bitmapLock（zHeapIterator.hpp:112），本轮不重复迁移。
排除：_relocated_fields_lock（#1314）、cacheMutex（#1317）。
