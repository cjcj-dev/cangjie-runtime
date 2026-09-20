LANE=sym_cangjie_runtime_740_implement_r5747608277
# #740 失败定位与边界确认
基线 b6d62daa8f3557c8a9effbfa4709a4744e315497 实读 rc=0。
kkk2:/root/diff_b6d62daa8f35/unit-default/run.log:4451,4455,4468,4533,4538,4557 显示六项失败均在旧值工作发布；pending=1 / remset=1 已成功。testable 五项在 run.log:4353,4372,4378,4388,4392 均为发布数 0。
源码证据：tests 在 MarkPublicationFixture 启动 mark 后 reset 页；runtime/src/Heap/z/zPage.cpp:266-284 reset 调 ResetPageSequence，zPage.inline.hpp:703-705 以页序号等于代序号判 IsAllocating；zMark.inline.hpp:19-21 跳过 allocating 页，与 ZGC /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zMark.inline.hpp:51-55 一致。初步因果假设为测试把 SATB 前存对象误布置到新分配页，准备对照复现。
拟在名单测试内把 reset 放在 MarkPublicationFixture 之前（不修改共享夹具），保留全部 prev/new/remset/domain 断言，并增加 allocating 页不发布对照。StoreFixture 成员初始化在构造函数前已开始 mark，拟仅在该文件本地修正顺序。
另实读发现 StoreBarrierBuffer::Flush :158-162 把 MarkObjectIfActive+remember 内联拆开，ZGC zStoreBarrierBuffer.cpp:277 调 mark_and_remember；拟恢复同一产品消费入口，删除该重复路径。
请确认：(1) 本地 fixture 顺序修正归本包可行；(2) #720/#736 是否正在碰 test_store_barrier_buffer.cpp / test_barrier_old_atomic.cpp / test_young_conc.cpp 上述区域，避免撞车；(3) 无需为 YoungPending=1 更改正确的 allocating 产品分路。
问题不依赖答复的工作：继续基线两构型构建与失败复现，读 ZGC producer-consumer 锚。
