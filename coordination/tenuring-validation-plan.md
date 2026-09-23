待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。
LANE=sym_cangjie_runtime_906_implement_r5785382414
ROLE=implement
PROGRESS=WIP

# 依赖裁决前方案（未执行，不构成测试证据）

基线 53371a6496e50c254e1c14abfc6824d4ef2a7bc4。
生产→消费顺序见本棒报告。产品代码保持未修改。

1. 默认配置：沿 InitCJRuntime -> HeapManager::Init -> ZArguments::initialize 完成初始化。读真实 SO 中最大阈值及 headroom 输入；覆盖小/大 heap、不同 worker 输入。阈值应为 ZGC 循环第一个使 per_age_overhead * threshold >= significant_young_overhead 的值，至默认上限截止。预期从独立整数边界计算，不调用被测 helper 求期望。
2. 覆盖 -1：走自动上限；覆盖 0：非 promote_all 相位返回 0；覆盖正值：即使默认自适应上限更小仍用显式值；promote_all 优先于覆盖。
3. 消费闭环：使用真实驱动 GC，相位 select_relocation_set 内调用的结果进入断言；不手工构造 TenuringInputs 再调用下游冒充集成。
4. 复用 runtime/tests/gc_unit/test_gc_director.cpp:117 的独立 VM 运行时夹具/GDB 输入配置方式；GDB 只改变初始化输入、观察产品状态，不改统计值与产品返回值。
5. 生产刀：切自适应上限赋值或覆盖来源，观察初始化结果目标断言。若只能切新增行，依常备裁决 3 单列。
6. 消费刀：切冻结基线 runtime/src/Heap/z/zGeneration.cpp:1165 的 SelectTenuringThreshold 调用，需产品阈值结果断言精确转红；切点已登记于 PHASE_ENTRIES.txt:71 的 select_relocation_set。
7. 同一 ELF、两 SO 血缘，绿 SO == 恢复 SO != 切刀 SO；断言输出和值/rc 都保存。测试前确认运行与加载身份。
8. 默认/testable 并行构建用 kkk2_build_two.sh；default/filler/testable 与 managed 差分用统一服务。OHOS 实际尝试，不能用 testable 代 filler。

尚需确定：参数公开输入形式与现有 GCParam 契约；新增 fixture 的可确定相位构造；#900 依赖授权。以上均未完成。
