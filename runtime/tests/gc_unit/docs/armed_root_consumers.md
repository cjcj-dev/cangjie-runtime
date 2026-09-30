待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# ArmedBadRootIsSkipped 的根槽窗口与消费者

坐标基于 `31d670c3ec5e2ff6b3b385d8c568b80de9502e75`。接回主线 `fc2fe6e2dc0b295e09a540d8e6c55717fa147718` 后复核（WrapperTask 新增 MRT_PreRunManagedCode，以下调度器锚按接回树）。本表是源码可达性，不是消费者命中的运行证据。结论：装置就位，待下一次命中；WrapperTask 为首要候选，不能据本表判定历史失败归因。

## 窗口与生产端

`runtime/tests/gc_unit/test_verify_fail_close.cpp:782-800`：MCC_NewCJThread 创建载体；DriverLocker 内只执行 young.pause_mark_start，随后释放锁；暂设 TLS 只为取得载体参数地址，再恢复 TLS；保存 obj，写入 0x1000，断言 armed，执行 old.pause_verify，检查值和 armed，最后恢复 savedObject。写入到恢复期间不是持有载体根锁的区间，pause_verify 的 DriverLocker 也不覆盖写入前后的全部窗口。

真实创建路径：`runtime/src/CjScheduler.cpp:406-423` 将 future 存到 LWTData.obj，传入 WrapperTask；`runtime/src/CJThread/src/runtime/schedule/src/cjthread.cpp:1152-1175` 建载体并写全局/本地队列（exclusive/UI 为不同调度分路）。本用例没有选择另一个任务函数。`runtime/src/CjScheduler.cpp:392-394` 先入口屏障，再读 obj，随后调用 GetTypeInfo，无空值分路。

物理根组：`runtime/src/Concurrency/CJThreadModel/CJThreadModel.cpp:56-61` 的 oops_do 精确暴露 obj/threadObject/execute 三槽。全载体枚举经同文件 :76-83 → `runtime/src/CJThread/src/runtime/schedule/src/schedule.cpp:1675-1698`；单载体枚举在 :1661-1670 持有 uncoloredRootLock。本测试直接写 obj 未取得该锁，因此不能仅据 armed 断言排除并发消费者。

## 机械枚举及调用层

可复跑（在上述坐标或其候选树）：

```sh
rg -n 'lwtData->obj|data->obj|data\.obj|RootSlotAt\(&.*obj\)|CJThreadGetArg' runtime/src --glob '*.{cpp,h,hpp}'
rg -n 'root\.oops_do|root\.entry_barrier|VisitCJThreadRoots|CJThreadVisitRoots' runtime/src --glob '*.{cpp,h,hpp}'
rg -n 'VisitGCRoots|VisitRootLists|ProcessFinalizableList' runtime/src/Heap/z/zReferenceProcessor.* runtime/src/Inspector/CjHeapData.cpp
```

直接 obj 读有 WrapperTask :393、WrapperExecuteClosure :522、WrapperClosure :655，以及 CJThreadRoot::oops_do :58。obj 初始化/存入行是生产端，不是读；Sync.cpp 的 CJThreadGetArg 路径读取 threadObject，不能当作直接读取本槽。以下展开 oops_do 的全部消费者与调用入口。

| 消费者/分路 | 读取/调用锚（runtime/src/） | 真实发起入口 | 窗口内源码判断 |
|---|---|---|---|
| 验证强根，armed / disarmed | Heap/z/zVerify.cpp:140-144 → CJThreadModel.cpp:58 | test :796 → zGeneration.cpp:902-909 → VM_ZVerifyOld :283-291 → zVerify.cpp:83-87,149-162 | 必经验证入口。armed 分路直接 return，不读组槽；若并发已 disarm，走 oops_do。测试断言不锁住整个窗口，不能据源码保证一直 armed。 |
| 验证弱根 | Heap/z/zVerify.cpp:164-169 | AfterWeakProcessing :87 | 遍历 colored weak storage，未接 carrier iterator，不直接读本槽。 |
| young 根标记，armed / disarmed | Heap/z/zMark.cpp:130-138,202-239 | zGeneration.cpp:408-413,418 → driver.cpp:98-100,257-261 | armed 时读三槽并发布部分颜色；disarmed 跳过。测试仅手动 mark_start，没有手动 concurrent_mark；后台请求获得 DriverLocker 后可运行，正文不能证明窗口内无后台请求。 |
| old 根标记，armed / disarmed | Heap/z/zMark.cpp:117-125,144-186 | driver.cpp:112-115,284-290 → zGeneration.cpp:828,864 | armed 读三槽并 disarm；与 young 同样为后台请求条件可达，非 pause_verify 直接启动。 |
| old 的 young 根 remap，armed / disarmed | Heap/z/zGeneration.cpp:921-930,935-980 | old.collect :845 → concurrent_remap_young_roots :977 | armed 读三槽并 disarm。依赖完整 old collection 的后续相位；本测试不直接启动它，后台请求条件可达。 |
| mutator 入口屏障，fast / slow | Concurrency/CJThreadModel/CJThreadModel.cpp:85-94 → :41-46,:63-73 | CjScheduler.cpp:392 | 已入队 WrapperTask 可在非 STW 区段运行。fast 不读组槽；slow 在锁内 recheck、process_weak 遍历三槽、disarm。第一次读取可能发生在此处，不能只盯住 :393。 |
| WrapperTask 直接 obj 消费 | CjScheduler.cpp:393-394 | CJThreadNew 指定的函数，CjScheduler.cpp:421 | 本载体唯一选择的 wrapper，入口屏障后读槽并解引用 TypeInfo；首要候选，待栈证实。也不能排除武装前或恢复后消费原始 nullptr；抓栈应保留实际 future 值。 |
| WrapperExclusiveClosure / WrapperOfExecuteClosure | CjScheduler.cpp:520-522 / :654-655 | 对应创建器 :542 / :678 | 两者也读 obj，但本载体函数是 WrapperTask，调度不将它替换为另两个函数，故不能读取本载体的该槽。 |
| StoreCJThreadObject 的 keep-alive 全组遍历 | Concurrency/CJThreadModel/CJThreadModel.cpp:98-111 | Sync/Sync.cpp:747 以下的 MCC_SetCurrentCJThreadObject | 当前载体入口屏障+全组三槽遍历，写的是 threadObject。测试在取得地址后恢复 TLS，未调用此 setter；需该载体进入此运行路径才可能间接读 obj。 |
| 堆迭代 full / strong+weak | Heap/z/zHeapIterator.cpp:215-223,227-250 | HeapIterator::push_roots，含 weak 的变体仍共用 strong carrier | entry_barrier 后遍历载体。本测试没有发起堆迭代；pause_verify 的 Objects（若开启）走对象页验证，不是此根迭代入口。 |
| inspector 并发模型根 | Concurrency/CJThreadModel/CJThreadModel.cpp:204-211 | Inspector/CjHeapData.cpp:365-377（ProcessRootConcurrencyModel） | entry_barrier 后遍历载体。用例没有请求 heap dump，因此没有该入口；不能从函数名把它算作 pause_verify 的消费者。 |
| mutator 栈 / watermark 根 | Heap/z/zVerify.cpp:159-160；zMark.cpp:156-158,213；zGeneration.cpp:953-956 | GC 根任务 / safepoint watermark | 遍历 mutator frame slots，不直接访问 LWTData.obj；若 WrapperTask 已加载 future，可能遍历其栈上副本，已先经过上述直接消费者。 |
| finalizer 对象 / 根列表 | Heap/z/zReferenceProcessor.cpp:719-746；zReferenceProcessor.hpp:104-106,131 以下；zMark.cpp:152 | finalizer Run/ProcessFinalizables；old 标记的 finalizer storage | 从独立 OopStorage/list 读对象，不枚举本 LWTData.obj。载体的 obj 没有注册到这些列表；finalizer 自有 CJThread（:781-799）不使它成为本载体。最终对象 GetTypeInfo 同名不足以认定此路径。 |
| static/native/weak colored 根 | Heap/z/zRootsIterator.cpp:115-135 | RootsIteratorStrong/AllColored | 独立 storage，不直接访问载体槽；若另有别名需另证生产端，本测试不建立这种别名。 |

## ZGC 对照

| ZGC 锚 | 不变量 | 我方锚 | 判定 |
|---|---|---|---|
| `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:354-359` | do_nmethod 在 armed 判断处跳过验证 | zVerify.cpp:140-144 | ✅ 同一 closure 分路，跳过 oops_do；不是入口屏障消费者的证明。 |
| `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zBarrierSetNMethod.cpp:78-91`（含 :53 起 slow path） | 入口 heal 根后 disarm，才允许后续对象消费 | CJThreadModel.cpp:63-73；CjScheduler.cpp:392-394 | ⚠ 入口行为尚未取得运行证据；CJThread 的 LWTData 根组是本树对 nmethod oop table 的映射，本条不以该映射宣称形态验收通过。分路与根组处理已定位，实际信号消费者待栈。 |

## 捕获的范围与限制

run_parallel_tests.sh 只将 main 的精确用例名送入 capture_segv.sh。GDB 从启动跟踪全部 fork/exec inferiors（detach-on-fork off，schedule-multiple on），SignalEvent 为 SIGSEGV 时先输出 siginfo、inferiors、thread apply all bt full、两代 _phase（有调试符号时），记录故障线程寄存器、PID/TID、inferiors/exec、停住进程的 /proc/maps 和实际映射二进制 sha256，随后正常递送信号。独立记录每个后代终态与捕获状态，并排空全部 inferiors 后报告 root 状态。外层 timeout、tally 与 PASS/FAIL 完成合同保留；缺 gdb 使该项失败，不能静默绕过捕获。GDB 会改变调度时序，未命中不是否定故障的证据。无 core，无手写 TLS，无直调产品消费者，无重复撞信号。
