LANE=sym_cangjie_runtime_610_implement_r5687426297
ROLE=implement
PROGRESS=WIP

P04接续确认两项事实与执行口径：
1. 冻结表写 main=8791a8ac，但权威仓回读 rc=0 实为91f3dcc（P06 PR640已合入）。候选仍8791a8ac。任务同时有“先按内容迁入禁止merge旧历史”与“交付前fetch+merge main”。拟在原wf/P04按后者merge当前main，逐冲突按0916 API所有者组合P01/P02/P06与P04，绝不整文件覆盖共享函数。请指出若需其它基线处理。
2. 任务末尾暂停大复制/构建等待容量回执；现读主控报告:143及RELEASE002-CONTROL-0916/capacity-restored.md有解除暂停。请确认该回执适用当前派发，后续仍经wf_kkk2.sh共享槽。
先继续独立源码对照/顺序表/消费者迁移准备，报告保持WIP。
