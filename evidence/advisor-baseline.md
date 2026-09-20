LANE=sym_cangjie_runtime_730_implement_r5746502983
ROLE=implement
PROGRESS=WIP
任务冻结表称权威 /root/cj_build/cangjie_runtime 的 cjcjdev/main=1d1cf0af83d05e8509f0cc575ec524a1a831dc30。实读 git -C /root/cj_build/cangjie_runtime rev-parse cjcjdev/main 返回 8cbac1ea8ef31dbaf5d49d3a1fe67300938ae74c，rc=0；本树 HEAD=1d1cf0af83d05e8509f0cc575ec524a1a831dc30，rc=0。
请确认：本轮仅补上轮已完成形态的断线证据，entry_cut_check --base 可使用派发候选 1d1cf0af（切线在该提交已存在），DIFF 基线继续使用实际 merge-base 主线？TLAB/对象分配真实入口 AllocateImpl/allocate_new_tlab/alloc_object 尚未登记 PHASE_ENTRIES，是否应由主控登记（本轮也会核既有 alloc_page/ZeroAndFill 入口是否覆盖）？
未改产品；继续整理调用链/目标测试。
