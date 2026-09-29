待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`
坐标基于 `9733dfc09d29eca27d19cc3937a149838b4e3322`。

| producer | consumer | 必须发生的顺序 | ZGC 锚 |
|---|---|---|---|
| zPageAllocator.cpp:327 ClaimPageMemory → :209 increase_capacity | :215 Cancel → zUncommitter.cpp:304 | 增容后立即重置 cache 水位，再记录取消时间 | zPageAllocator.cpp:648-658; zUncommitter.cpp:286-290 |
| zUncommitter.cpp:209 flush 为 0 | :211 Cancel | 重置水位在取消时间之前 | zUncommitter.cpp:395-400 |
| Cancel 的 cache 水位/取消时间 | Activate :169、:179 | 已取消则更新下周期 timeout、reset 并返回 false；未取消则读取水位生成预算 | zUncommitter.cpp:205-243 |
| Uncommit/RegisterUncommit 的预算/进度 | run_thread :283 → deactivate/reset/next-timeout | active 且 finished/canceled 才能 deactivate；超时更新先于 reset | zUncommitter.cpp:109-169,177-284 |

改动前消费者机械检索：`rg -n 'Cancel\(' runtime/src` 仅 zPageAllocator.cpp:215、zUncommitter.cpp:211 两处调用，另含定义与声明。真实入口为 ClaimPageMemory 的分配链及 run_thread 的后台线程链；Uncommit 在 PHASE_ENTRIES.txt:53 登记。

本轮 Wait 返工（坐标 c3f54ca245a535d247d995fd2671954a067ec00a）：
run_thread → Wait(nextCycleTimeout / nextUncommitTimeout) → TimeUtil::NanoSeconds 秒值 → ToMillis(waitUntil - now) → condition.wait_for。
ZGC zUncommitter.cpp:72-85：now/wait_until 均 double 秒，毫秒消费前统一 to_millis。仅修此数据表示，不改共享锁。
