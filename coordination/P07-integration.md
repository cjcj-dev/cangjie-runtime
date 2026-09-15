待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。
# P07 接续 / producer→consumer 顺序
坐标：候选91bb7d30e1e040eb6c6c8386dcdc0c90fbf0ae11，主线91f3dcc232201d4ad98ec3af6f10165edb950416。

| producer | consumer | 整合不变量 |
|---|---|---|
| zDriver.cpp StartGCThreads 的 ConcGCThreads 赋值（主线） | zMarkingSMR.hpp 的 ZPerWorker 构造、workerThread.cpp:58 TLS id→task.work | 预算先于初始化；最终派发器拥有TLS赋值，保留P06工具族 |
| zWorkers.cpp:77 run 的 at_start | workerThread.cpp:32 coordinator_distribute_task → :69 完成计数 → :74 end信号 → zWorkers.cpp at_end | 仅全部参与者完成才返回；R3切consumer完成信号 |
| zMark.cpp MarkOldRootsTask::work 尾部FlushCurrentThreadMarkStacks | zGeneration.cpp testOldMarkStarted → root_publication_snapshot.hpp:29 VisitList | 保留测试侧真实发布观察窗，迁P02 entry API；保留MajorSeed原判据 |
| P06 zRelocate.cpp ZArrayParallelIterator | 各ZTask::work、ZWorkers::run | 保留迭代器，改调度公共API；不恢复旧原子下标分发 |
| P02 zGeneration.hpp sequence=1、live map与MarkStackEntry | P07 generation workers 与任务体 | 逐hunk组合，不恢复旧MarkView或条目字段 |

基础原语Semaphore使用P06已合入定义，保留trywait消费者；P07只消费signal/wait。
