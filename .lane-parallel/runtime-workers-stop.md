# Runtime workers 的 native teardown 闭环切口

产品改动已落盘，未构建/测试/commit/push。`git diff --check` rc=0 只代表补丁形式。

## 事实与 ZGC 锚

JDK zCollectedHeap.cpp:95–111 的 stop 遍历GC线程但只对 ConcurrentGCThread 停止；shared/workerThread.hpp:86 起的 WorkerThreads 与 shared/workerThread.cpp:219 的永久工作循环不提供进程内池析构停止。zRuntimeWorkers.cpp:30–42 构造线程池。这是 HotSpot 进程退出与 Cangjie 原生 runtime Fini/进程内夹具销毁的基础设施差异。

我方 ZCollectedHeap 是 ImmortalWrapper 持有，runtime native teardown 不触发其成员析构；因此只依赖 WorkerThreads 析构不能关闭该真实 owner 的 runtime pool。该源码缺口与此前目标 PASS/sentinel 后出现的进程异常没有已证明的因果关系，本报告不声称已修复那两次异常。

## 修改

- workerThread.cpp:90 析构调用 stop 后释放指针数组；数组创建时零初始化。
- workerThread.cpp:96 的新 stop 使用 `_stop_lock` 串行并发 stop，复用原 WorkerThreadExitTask→dispatcher→pthread_join；不改变派发、worker执行或结束信号协议。
- 每个已删 worker 指针置 nullptr，active=0，再 release-store created=0；第二次 stop 不再派发任务；initialize_workers 可在 owner 静止后重新建立同一池。
- workerThread.hpp:98–102 写清停止/初始化/重启和任务派发的 owner 协议：必须先停止任务协调者；本锁只保证并发 stop 幂等，不冒称允许 stop 与活跃 dispatch 并发。
- zRuntimeWorkers.hpp:15 `stop()` 直接操作 `_workers.stop()`。
- zCollectedHeap.cpp:184–188 在 drivers、generation workers、stat、dedup 已停止后显式关闭 `_runtime_workers`，随后 native runtime 方可继续拆其它服务。

CLAIM: runtime pool 停止入口作用于 ZCollectedHeap 真正持有的 WorkerThreads，而不是观测副本。
  METHOD: read
  EVIDENCE: runtime/src/Heap/z/zCollectedHeap.cpp:188;runtime/src/Heap/z/zRuntimeWorkers.hpp:15;runtime/src/Heap/z/workerThread.cpp:96

## 待父棒统一验证

测试 agent 已收到接口：真实 ZCollectedHeap::stop 后 safepoint_workers 的 created/active结果；独立池 stop幂等及initialize_workers重启；产品起停因果测试和三臂结果。不将源码可达性写成运行闭环成立。

## FALSIFIED

无；历史异常原因仍未证实。
