待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。
基线 55b1f45b6ca7bf48485893faa3a76f07f576e31a。

| producer | consumer | 顺序与修改点 |
|---|---|---|
| zCollectedHeap.cpp:57 _initializer → zInitialize.cpp:22 | zDriver.cpp:49 lock | 初始化 ZLock 指针必须在任何 driver 构造/启动前；ZGC zInitialize.cpp:69 |
| zDriver.hpp:357 minor 构造 | zDirector.cpp:446,596; zDriver.cpp:233; zPageAllocator.cpp:657,762 | 构造内登记必须先于 create_and_start；ZGC zDriver.cpp:124 |
| zDriver.hpp:368 major 构造 | zDirector.cpp:449,466,578,585,588,608; zDriver.cpp:205,214,237; zPageAllocator.cpp:764 | 同上，ZGC zDriver.cpp:325 |
| zDriver.cpp:112 is_busy 返回 port 状态 | zDirector.cpp:446,449,466,585,588,608 | 决策消费者通过注册访问 driver，busy 结果进入实际决策；ZGC zDirector.cpp:801-819 |
