LANE=sym_cangjie_runtime_739_implement_r5747765432
fd2886e3c17effa113c65e91669a5e2568fe1cc9 四臂差分已完成：default/filler/testable/managed CAND-ONLY=0；报告 /root/cj_build/reports/DIFF-fd2886e3c17e-vs-b6d62daa8f35.md。
但故障对照固定ELF每例N3的绿/恢复臂发现真实新失败，不重跑覆盖：
- restored-testable/ProductReturnedCapacityServesOnlyOneWaiter.1
- restored-testable/ObjectAllocatorPaths.MediumBlockingFailureAttemptsCollection.3
两者均命中 runtime/src/Heap/z/zCrossVM.cpp:458 CHECK(discoveredExternObjects.empty())；远端日志 kkk2:/root/sym_cangjie_runtime_739_implement_r5747765432-evidence/arms/restored-testable/<完整用例名>.<样本>.log。
源码指向共享 discoveredExternObjects 跨 old/minor 周期竞争（需进一步取证），不是本轮修改的队列字段。请裁定该前置是否纳入本条，或单独立项并本条按证据不足收口；不能据单次四臂0独红覆盖这两次真实失败。
另：为 reset 故障对照在测试拿 driver lock 时，我的观察hook停在major young根扫描（old snapshot已Mark），自身阻塞到30s，已由gdb定位这是测试构造错误。现按young.YoungType()==none筛到实际old扫描，再构造stop；不改产品迎合测试。
