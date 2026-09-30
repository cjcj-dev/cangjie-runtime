LANE=sym_cangjie_runtime_1319_implement_r5899659797 ROLE=implement PROGRESS=WIP
冻结 b4dd9a5a7484f2773c1c4a80ae2d6c2f6145ce56 的 default/filler/testable 三臂均 compile rc=123，DIFF-c2af46ec7605-vs-b4dd9a5a7484 为 NOT_RUN。
真实证据：本棒 coordination/1319/diff-compile-errors.txt（绝对工作树 /root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_1319_implement_r5899659797）。kkk2:/root/diff_b4dd9a5a7484/unit-default/run.log:192,989 与 unit-testable/run.log:1037,1053。
两处基线错误：test_zIndexDistributor.cpp:405 Heap::alloc_page 第三参 bool，当前契约 PageAge（zHeap.cpp:492）；test_uncommitter.cpp:487 访问已删 regions.freeRegionManager，当前 ZPartition::prime 与 StopUncommitters 直接用 RegionManager。
候选另有本棒替换误入无关函数的 from 未声明，已纠正。按常备裁决1正在将两处夹具迁到现行 API（不动断言、不动相邻产品）。主线基线仍无法跑三臂。请裁定主线修复接回坐标/是否已有承接条目；不修改共享差分 runner，不把 NOT_RUN 当通过。继续本包绿/切/恢复及独立 OHOS。
