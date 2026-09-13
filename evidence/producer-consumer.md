待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。
基线 41b05e57b13c02aff9792588fcb97a4ba5161736，先于产品修改记录。
|生产端|当前消费端|必须迁移的位置|
|Base/LogFile.h:279,341 Timer|Base/ZStat.cpp:204 NotePhase → :229 cycle clear|静态 phase 身份在采样前构造；Timer 直接引用身份，tick 消费常驻 sampler|
|CollectorResources.cpp:360 cycle.AtStart/AtEnd|Base/ZStat.cpp:94 SampleDirectorStats|保留 per-generation 周期对象；collection 包住整个 driver 请求|
|MutatorAllocRate.cpp:136 sample_allocation|:178 stats → ZStat.cpp:100|本体迁 ZStat；allocator 生产出口改引用新类|
|GcStats.cpp:135,151 completion|ZStat.cpp:140-143 director|被覆盖的重复序列删除；heap stats 迁 ZStat 按代存储|
|CollectorResources.cpp:96 initialize|统计线程 → history → 输出|独立 1 Hz 线程初始化 per-CPU 数据，终止时 join|
