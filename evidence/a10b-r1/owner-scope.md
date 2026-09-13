LANE=sym_cangjie_runtime_494_implement_r5655968840
ROLE=implement
R1 pinned owner 修复在做，另枚举 GetMutatorPhase 得到：
1. zMark.cpp:1343 WCollector::MarkNewObject 仍读线程单值，BaseObject.cpp:182 有调用（审查说未调用定义，实际存在调用表达式）。拟按 ObjectGeneration(obj) 读取 GetGCPhase，既有 MarkObject 算法不变。
2. Interpreter/InterpreterSpecific.cpp:499 IsActiveGCPhase 回调在 :651 注册，返回 mutator->GetMutatorPhase() >= ENUM。此接口无对象参数，拟改读 YOUNG/OLD owner phase 的同一谓词并取 OR，保留 tld/mutator 空值检查，不改解释器机制。
3. MutatorManager.cpp:1213 是握手完成检测，应继续读取操作上下文而非 generation 状态。
请批准 #2 文件直接 owner 消费传播范围，并确认聚合两代 active phase 的语义；#1 已在本包 zMark 范围。不会新增开关或恢复整周期串行锁。
