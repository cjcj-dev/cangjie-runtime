LANE=sym_cangjie_runtime_730_implement_r5746042754
ROLE=implement
PROGRESS=WIP
# #730 分段数组 receipt 删除碰到邻包消费者
任务书要求删除 ZGeneration::testOldMarkStarted/testYoungMarkCompleted。
实读除 test_segmented_array_init.cpp 外，还有 test_p2_field_barrier.cpp:296,325,439,455、test_native_root_current.cpp:230,478、ohos_host/ohos_cycle_unit.cpp:89、test_generation_cycle_context.cpp:211。
这两个回调删除会令上述邻包无法编译，重写这些机制测试超出本条分配栈边界。请明确：邻包同步迁移由谁负责，还是本条允许保留该二者的邻包消费并仅删除本包消费？不擅自用空壳替代或删邻包断言。
本条继续共享页/flags 测试和本包 segmented 迁移，报告保持 WIP。
