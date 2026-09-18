# Allocator 所有权切口：唯一值所有权已实现，待父棒统一构建

父棒读过第一版方案后明确授权一个 WIP 切口：Heap 直持真实 RegionManager，RegionSpace 对象分配适配留待后续；允许 RegionSpace.cpp 与必要 inline 解环。该切口现已落盘；未运行构建、未 commit/push。zHeap 文件独占已经释放。下方“原状/完整方案”保留为为何未宣称完成全部折叠的说明。

## 本次已实施

- zHeap.hpp:274：`RegionManager _page_allocator` 真正值成员；:276 的 `_allocation_adapter` 仅持 RegionSpace 对象适配，不再以 page allocator 名义包装。
- zHeap.cpp:136 页初始化直接走成员；:282–301 容量/地址/使用量直接读成员；:514 页分配真实执行直接取 `page_allocator()`。
- RegionSpace.h:64 的 GetRegionManager 为 out-of-line 声明，RegionSpace.cpp:30–33 返回 Heap 唯一页分配器。原 RegionManager 值字段已删除，未加兼容别名。
- zPageAllocator.hpp:778–789 所有 reserved range、metadata mapping、虚拟/物理 backing 及 cache 都同属 RegionManager；声明序使 cache 先析构、backing 后析构。RegionSpace 原资源字段删除。zPageAllocator.cpp:1240–1309 保留资源初始化及析构行为，所有者变为真实页分配器。
- 解环：zPageAllocator.inline.hpp 三个直接依赖 Heap 的非模板定义原文移动到 zPageAllocator.cpp:1313/1343/1351；zObjectAllocator.inline.hpp/zRelocationSet.inline.hpp include 挪到 zHeap.hpp 类定义后，避免值成员所需完整类型与 Heap inline 的循环。
- 夹具字段迁移交测试 agent；已提供 `friend GcUnit::GcHeapFixture`，由其改两行 reserved 字段访问。
- 检查：`git diff --check` 已执行 rc=0；未编译，不能宣称可编译或测试通过。父棒统一双构型/差分验证。

## 本次界限

仍存在 Allocator/RegionSpace 对象分配、buffer manager 与 uncommitter 中转，不作为完成 #700 全范围的证据；本切口只证明 Heap 真正值持唯一页分配器及页资源。Uncommitter 目前经对象适配的 GetRegionManager 访问同一个 Heap 成员；未改它的线程/停机行为。


## 修改前所有权，不是命名问题

`Heap::_page_allocator` 是 unique_ptr<RegionSpace>（zHeap.hpp:272）；RegionSpace : Allocator（RegionSpace.h:33），继承 AllocBufferManager/Uncommitter（Allocator.h:91–99），自身持 virtualMemory/physicalMemory/RegionManager（RegionSpace.h:246–273）；RegionManager 持 FreeRegionManager（zPageAllocator.hpp:769）。因此把 RegionSpace 原样改名 ZPageAllocator 或让 Heap 直接值持 RegionSpace 都没有兑现“去包装及中转”。

JDK zHeap.hpp:48–56 直接值持 ZPageAllocator/page_table/object_allocator/serviceability/old/young；zHeap.cpp:59–70 在构造参数中配置 page allocator，zPageAllocator.hpp 的 ZPageAllocator/partition 是页资源所有者。必须把页资源与对象分配/TLAB 路由分开，不能搬一整个混合 RegionSpace 类。

## 最小具体实现顺序

1. **定义最终接口及消费分流**：Heap 提供具体 `page_allocator()`，对象申请留 Heap→ZObjectAllocator，TLAB 注册/遍历归对象分配或现有线程缓冲组件；不保留 `Allocator& GetAllocator()`。把静态 `RegionSpace::GetAllocSize/ToAllocSize` 消费放到已有对象大小工具，mark helpers 消费归 ZPage/ZMark；不能新造兼容别名。
2. **页资源合并**：将 RegionSpace 的 virtual/physical manager、metadata mapping、reserved range 生命周期移入真正页分配器。RegionManager 已有页分配/容量/缓存执行逻辑，需合并其资源所有权而非再套新 ZPageAllocator 壳。Heap 值持这一个对象。RegionSpace::Init 的映射/初始化主体位于 zPageAllocator.cpp:1240–1294，依赖 runtime log/GC 参数，必须和 bootstrap 接口协作。
3. **对象路由分离**：zObjectAllocator.cpp:340/359 的 RegionSpace::TryAllocateOnce/Allocate 已物理位于 zObjectAllocator 文件，但逻辑仍挂 RegionSpace；迁入 ZObjectAllocator 并从 Heap::Allocate 调用。RegionSpace.cpp:35 的 FeedHungryBuffers 及 Allocator.cpp:21 的 AllocBufferManager 初始化须同步迁移到其真正 owner。
4. **uncommit 反向引用切换**：zUncommitter.hpp:27/:68 当前接受并保存 Allocator&；cpp:181/:200/:298 依靠 static_cast<RegionSpace&> 才获得 RegionManager。必须改为直接页分配器引用。否则删除包装不可能保持可编译，也不能用兼容壳“过渡”交 DONE。
5. **消费者全量切换及删类**：产品直接消费者见下检索；测试由独立 agent 同时修改。移除 Allocator.cpp:79 NewAllocator 工厂与 RegionSpace.cpp 方法、更新 CMakeLists 和导出文件；不得仅删 include 然后保留旧符号。

## 超出当前独占文件集的必要文件

- `runtime/src/Heap/z/zUncommitter.{hpp,cpp}`：直接所有权依赖，非可选。
- `runtime/src/Heap/Allocator/{Allocator.cpp,RegionSpace.cpp,CMakeLists.txt}`：真实函数定义及构建输入，非可选。
- 消费者 `runtime/src/Heap/z/{zGeneration.cpp,zDriver.cpp,zDirector.cpp,zCollectedHeap.cpp,zTracing.cpp,zMark.hpp,...}`：父棒/其它 agent 当前域；不能无协调批改。
- `runtime/src/CangjieRuntime.cpp`：bootstrap 时序和 allocator 统计输出。
- `runtime/tests/gc_unit/`：局部实例构造/类型签名需同步。

枚举命令（已执行，rc=0）：`rg -l 'RegionSpace|GetAllocator\(' runtime/src`。本次输出命中 36 个文件，但仅是文本候选集合，含 CMake/导出和注释；不宣称 36 个实际调用者。完整列表可重跑导出，尚未逐项做语义确认。

## 并行安排

父棒保留 Heap/Generation/Mark 状态迁移。需要真实 allocator 切口时先冻结 page_allocator/对象申请/TLAB 三类目标接口，然后三臂并行：(A) 页分配器所有权+uncommitter+删除旧类及构建输入；(B) 产品其它消费者按机制改路由；(C) 测试夹具。A 与当前 zHeap/启动链存在直接交集，不宜现在盲写。可编译整合后统一双构型，不能将各半成品分开宣称通过。

## FALSIFIED

无；记录的是任务授权内允许的完整切口过大分支，未以机械改名伪装完成。

## 首次统一构建后的头解环修复

父棒两构型首次构建均失败，原文摘要 `.lane-parallel/build-owner-mark.log`，远端 `/root/sym_cangjie_runtime_700_implement_r5723534433-owner-mark/default-configure.log:459–475` 经 box.sh 读取（首次沙箱网络 rc=255，显式授权后 rc=0）。真实链：Mutator→Allocator→AllocBuffer→RegionList→zPage→zForwarding→zHeap→zPageAllocator→ScopedObjectAccess→Mutator.inline，出现未完成 Mutator 类型。

已移除该传递环：
- zForwarding.hpp 不再包含 zHeap.hpp；young_seqnum 定义移到 zForwarding.cpp。
- zPage.inline.hpp 不再包含 zCollectedHeap.hpp；GetFromPageCarrier/ClearUnits 两定义移到 zPage.cpp。
- zPageAllocator.hpp 不再包含 ScopedObjectAccess.h 或未使用的 Allocator.h；AddMarkQuarantineUnits 移到 cpp。
- zPageAllocator.inline.hpp 的 LockRegionListInSaferegion 移到 cpp，保留循环/安全区逻辑逐句不变。

RegionManager 值构造的前置检查：zObjectAllocator.cpp:387 构造只初始化成员容器及两个 TLAB 平均数 Sample；zTLABUsage.hpp:22 的 Sample 仅更新数值，无 Heap 单例调用。此为 read 证据，不替代构建/运行证明。

父棒继续统一重建；本子任务无 build/commit/push。

## 第二次构建后的头依赖修复

`.lane-parallel/build-owner-mark2.log` 两构型 configure rc=1。box.sh 读取远端 `owner-mark2/default-configure.log:402–415` 的首个错误栈：Mutator→StackWatermark→UncoloredRoot.inline→Barrier.inline→CollectedHeap→Heap→PageAllocator.hpp:376→Sync.h→CjScheduler.h→sema.h。远端无 rg（rc=127），改 grep 原文读取 rc=0。

zPageAllocator.hpp 声明及对应 inline 不使用 Sync.h 定义，已删除该无用 include；cpp 自身已有 Sync.h，保留真实产品依赖。未添加任何全局/局部 include path。父棒统一再次构建。
