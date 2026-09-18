# 构造创建 GC 线程：WIP 产品切口

未运行构建、测试、commit、push。`git diff --check` rc=0 仅证明补丁空白形式；本切口等待父棒统一验证。

## ZGC 锚与当前对应

| ZGC | 当前产品 | 改动 |
|---|---|---|
| zCollectedHeap.cpp:62–71 | runtime/src/Heap/z/zCollectedHeap.cpp:55–64 | 构造初始化列表 new minor/major/director/stat；初始化方法不再重复 new |
| zDriver.cpp:118–127、319–328 | runtime/src/Heap/z/zDriver.hpp:359、369 | 每个派生 driver 构造体启动已完成成员构造的对象，删除独立 start 包装 |
| gc/shared/concurrentGCThread.cpp:38–48 | runtime/src/Heap/z/concurrentGCThread.cpp:84–101 | run 在 run_service 前等待真实初始化完成事件 |
| runtime/init.cpp:245–258；runtime/threads.cpp:703 | runtime/src/CangjieRuntime.cpp:162–164；concurrentGCThread.cpp:37–43 | cjRuntime->Init 完成、g_initialized release-store 后，在完成monitor内release发布并notify |
| gc/shared/concurrentGCThread.cpp:55–69 | concurrentGCThread.cpp:106–129 | 保留service终止/join；同一完成monitor内置terminate并notify，允许初始化前取消等待 |

CLAIM: 产品服务线程的完成等待被接在真实 run_service 前，完成生产端位于完整 runtime Init 返回之后。
  METHOD: read
  EVIDENCE: runtime/src/Heap/z/concurrentGCThread.cpp:84;runtime/src/CangjieRuntime.cpp:162

## producer → consumer 顺序

1. ZCollectedHeap 构造 `_heap`，继而创建/启动两个 driver、director、stat。
2. 新线程 entry 设置线程类型/名称；run 锁初始化 monitor，等待 completed acquire 或 should_terminate acquire。
3. 当前产品 Heap::Init 完成页/代/worker 资源，runtime 其它 manager 完成 Init。
4. CreateAndInit 写 g_initialized 后 NotifyRuntimeInitialized，在同一 monitor 内 completed release-store，通知全部等待者。
5. 服务线程 acquire 观察完成后才运行 run_service；不拿 Runtime 指针非空作完成判据。
6. stop 即使早于第4步，也在同一 monitor 内置终止、notify，线程跳过service并写has_terminated，stop随后join。wait使用谓词，不依赖通知时刻猜测。

## 仍未完成的范围

- Heap 的完整资源尚未前移构造，该任务依旧 WIP；此补丁只将线程创建点及等待分路对齐。
- RegionSpace/Allocator 对象适配仍存在，由父棒继续处置。
- initialize_gc_workers 保留现有 worker setup 判断及预算计算，未改变 worker数量策略。
- 构造前全局 monitor 通过 ImmortalWrapper 获得进程生命周期；standalone fixture 没创建完整Runtime时线程保持等待，避免静态condition_variable析构与驻留等待者相遇。未添加测试开关或基于 Runtime非空的放行口。

## 尚需验证

真实产品起停、初始化前stop可完成、已初始化后的service进入、两构型及三臂统一回归；本文件不宣称以上测试通过或红臂闭环成立。

## FALSIFIED

无。
