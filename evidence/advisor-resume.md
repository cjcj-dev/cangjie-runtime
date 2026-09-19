问题：前轮候选 register_found_old 在未 bind 时提前返回（zRemembered.cpp:161），与 ZGC zRemembered.cpp:385-387 无该分路不一致，且主线新 GcHeapFixture 仅 bind_test_page_allocator（zHeap.cpp:511）。应如何消除此测试驱动产品旁路？
方案 A：产品未绑定 CHECK，GcHeapFixture 按真实 page_table/old forwarding 初始化并 bind remembered，销毁时恢复，相关共享 fixture 文件需授权。
方案 B：按 ZGC 将 found_old 生命周期迁到 generation 构造，但我方 page map 在 Heap::Init 后才有尺寸，须连同初始化层调整。
倾向 A，先确保 fixture 与真实产品持有同一页表粒度，不保留跳过登记；请求指定共享 fixture 范围，或给 B 的初始化合同。当前已 merge c639ce5d，head de3c3cd9，正在独立构建，尚未修改该产品分路。
