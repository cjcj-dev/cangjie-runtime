LANE=sym_cangjie_runtime_614_implement_r5687433872
ROLE=implement
PROGRESS=WIP

P08开工核对：候选 b38fcfdef926f6ae5dff2e8d0ffa8f4d6356bc18；指定 runtime 主树 cjcjdev/main=91f3dcc232201d4ad98ec3af6f10165edb950416，各 rc=0。
1. 任务书要求 LLVM 同批 PR/联合 bundle，但冻结坐标表只有 runtime。请确认 LLVM 权威绝对主树、基线 SHA、候选分支/独立工作树，以及可复用 LLVM/SDK 构建产物坐标。拟从 /root/cj_build/llvm_rebase 正式 main 建本棒独立工作树，绝不动主树；未确认前仅阅读既有 P01 ABI。
2. 正文末尾暂停新大复制/构建直到容量恢复回执；controller 报告曾记录可用10.9GB解除暂停，但不能确认是否早于本轮再次告警。请确认当前可恢复或需继续等待。
3. #607共享函数最终会合请给当前候选 SHA/报告，P08按最终内容迁保留 Finalizable root/field、remset整链；不恢复旧实现或弱化断言。
目前继续做不依赖上述答复的 ZGC逐函数对照、producer→consumer 表与删除清单，不改范围。
