# ZCollectedHeap 构造启动的依赖核查（只读规划）

以下为当前工作树源码读证；估计轮数不是实测，每轮指一次可编译实现及双构型/三臂验证。

| 子任务 | 触及文件（runtime/src 下，除另注） | 依赖哪个子任务先完成 | 估计轮数 |
|---|---|---|---|
| 参数先配置、唯一 Heap 显式构造、消除 lazy singleton 再入 | CangjieRuntime.cpp、HeapManager.cpp、Heap/z/zArguments.{hpp,cpp}、zHeap.{hpp,cpp}、zCollectedHeap.{hpp,cpp}、zPageTable.cpp | HeapGcState/allocator 所有权的 zHeap 字段及构造接口先稳定；可先只读设计 | 1–2 |
| GC 线程 VM 初始化完成屏障 | Heap/z/concurrentGCThread.{hpp,cpp}、CangjieRuntime.cpp（发布完成）；测试 bootstrap 发布入口 | 前项的构造/完成事件契约先确定；与核心所有权产品文件不相交，但与启动任务 CangjieRuntime.cpp 相交 | 1（可并入启动轮） |
| driver/director/stat 改构造创建并启动 | Heap/z/zCollectedHeap.{hpp,cpp}、zDriver.hpp/.cpp；必要时 zDirector.cpp/zStat.cpp | Heap 在构造返回前完成 page allocator/table/generation/workers；上项等待完成屏障；测试 bootstrap 同步 | 1 |
| gc_unit 唯一 Heap bootstrap 与夹具 | runtime/tests/gc_unit/gc_heap_fixture.hpp、mark_publication_fixture.hpp 及检索得到的直接初始化用例 | 可并行做 GetCollector→Heap API 消费替换；最终 bootstrap 依赖前项显式创建接口 | 1–2，与产品轮共同验证 |

不能把前三行全部按文件不相交并行编辑：它们有明确 zHeap、zCollectedHeap、CangjieRuntime.cpp 交集。建议父棒持有 zHeap/zCollectedHeap/Arguments/HeapManager/runtime 启动链；独立 agent 持有测试目录；另一个 agent 可在冻结接口后持有 concurrentGCThread 两文件，父棒只做完成事件发布。无需主控另开棒；若确需另开，必须独立 worktree/候选分支并以同一稳定接口 commit 为基线，最后由本候选分支按文件吸收，不触及 main。

## 初始化顺序与锚

1. CangjieRuntime.cpp:159–163 创建 runtime 并先发布 Runtime::runtime，再调用 Init；:181 构造复制整个 RuntimeParam，:203 初始化 PagePool，:211–218 建 manager/concurrency/signal，最后 HeapManager。HeapManager.cpp:20 当前 `Heap::GetHeap().Init(param)`，参数到达前 GetHeap 已触发构造。
2. zCollectedHeap.cpp:45–52 的 ImmortalWrapper lazy 构造；:55–64 当前线程字段 nullptr；zHeap.cpp:108–117 建 RegionSpace 与 HeapGcState；:131–157 之后才初始化 arguments/allocator/table/generation/workers。:300–302 的 Heap::GetHeap 又回 ZCollectedHeap::heap。
3. zPageTable.cpp:18 / :23 在表安装/获取时调用 Heap::GetHeap；:51/:70 注册 young remset 亦走单例。将 allocator Init 搬入 Heap 构造而不先改 singleton 发布，会有重入路径。Heap::_heap 可承担具体 Heap singleton，但需所有消费者在对象字段可用后才访问。
4. zCollectedHeap.cpp:95–143 的启动计算依赖初始化后 max capacity 与 region bytes，并绑定 reference processor workers，不能直接原样挪到 member initializer 前。
5. zDriver.cpp:56–60 构造只 set_name；zDriver.hpp:359–360/:370–371 子类分离 start。port.receive 是 driver 第一条等待（zDriver.cpp:65），请求后才访问全局。ZDirector 构造已立即 create_and_start（zDirector.cpp:68–73）；循环 :647 只检查 Runtime::CurrentRef 非空及 GCEnabled，不代表整个 runtime 已就绪。ZStat 构造 :407–412 初始化统计并起线程，:425 起 run_thread 不直接取 Heap。
6. concurrentGCThread.cpp:56–60 明写省略 HotSpot wait_init_completed，因为原先在 Heap::Init 后启动；改构造起线程时该前提失效。JDK gc/shared/concurrentGCThread.cpp:38–43 是 run→wait_init_completed→run_service；runtime/init.cpp:245–257 是等待/发布；runtime/threads.cpp:703 在启动末尾发布。
7. gc_heap_fixture.hpp:299–315 先建地址域与统计/手工 mapping，后 GetHeap，而没有完整 Runtime。不能让该 GetHeap 悄悄建默认容量真实 allocator；需统一显式 bootstrap，保留 fixture 对其页的局部构造契约。

## ZGC 对应

JDK memory/universe.cpp:895 先 initialize_heap_sizes，:961–963 显式 create_heap；zArguments.cpp:243–245 返回 new ZCollectedHeap。zCollectedHeap.cpp:62–71 初始化 barrier/initializer/Heap 后 new 两 driver、director、stat。zHeap.cpp:59–94 构造持有 page allocator/table/object allocator/serviceability/old/young，发布 singleton 后 prime cache 并置 initialized。故最小顺序为：参数前置→唯一对象显式构造及 field-ready 单例读取→Heap 构造完成资源→线程初始化等待/发布→在 collected heap 构造创建线程→测试 bootstrap。

## FALSIFIED

不能据 ZDriver.cpp:53 的注释“each driver ... starts in its constructor”判断现有代码已对齐：紧接 :56–60 构造实际仅 set_name，start 仍由 zCollectedHeap.cpp:142–143 调用。此为源码注释与执行代码不符，不是对任务本身的否定。
