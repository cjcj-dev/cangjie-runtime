LANE=sym_cangjie_runtime_606_implement_r5674249495
ROLE=implement
常态 runner 实测：首轮托管 N=3 均在 test_generation_cycle_context.cpp 编译失败，既有 YoungPreludeRequest 和 RootSlot::GetTargetObject 已移除（真实日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5674249495-green2/managed-0.log:55/58）。拟最小迁为 cycle.IsMajorRoots() 与只读 raw(slot.LoadPlain())，保留所有原 ASSERT，后者仅读并比较根身份不送 mark。该 runner 的根枚举机制属 P3/#603，若后续有其未达原断言的失败，按合同报告但不弱化，需你裁定归属；本次只修旧 API 编译接线以真正运行 R2。另合成周期 helper 定义收进 gc_unit_main.cpp 测试 TU（唯一宏 friend），不在产品路径保留 AdvanceSequence；会逐项列装置例外。
