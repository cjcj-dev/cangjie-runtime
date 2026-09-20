待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。
坐标基于 b6d62daa8f3557c8a9effbfa4709a4744e315497。
| producer | consumer | 顺序与修改落点 |
|---|---|---|
| zObjectAllocator.cpp:221 medium 阻塞回退 | zPageAllocator.cpp:831-838 入队及等待 | 失败 claim 后同一判断决定入队并等待，替换双 IsGcThread 分路 |
| zPageAllocator.cpp:834 入队 | zPageAllocator.cpp:655 GC 请求 | 请求保存 young/old seqnum，异步 minor 请求先于唯一 Wait |
| zDriver.cpp:84 ExecuteDriverRequest | zDriver.cpp:85 ack → :91 SatisfyStalledAllocations | 保持 driver lock 覆盖 ack 与失败答复/重启，失败按 old seqnum，晚到者留队 |
| zPageAllocator.cpp:707 used 减少 | zPageAllocator.cpp:710 SatisfyAvailableLocked | 容量归还成功答复与独占 claim 同锁；保持 ZGC :2164-2189 生产消费关系 |
| zCollectedHeap.cpp:164 abort | zDriver.cpp:80 reset / :82 abortpoint | 正在向 advisor 核对停止后不得重置 abort；退出排空属本地适配待裁定 |

ZGC 锚：zPageAllocator.cpp:1436-1465、1518-1542、2164-2189、2295-2363；zDriver.cpp:193-225、434-484。
