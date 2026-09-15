## 改动

Refs #606。位图原语统一为 true=newly-marked，并迁移页层与调用者；每代 mark-start 按 ZGC 执行颜色切换、退休、seq、phase、domain 与 young remset flip。旧代候选选择移到 mark 后，保留 owner CHECK。合入 #596 后，其已获资格 young 根接入统一 Generation 入口。

## 验证

- default/testable 构建 rc=0；default/filler 533项中532通过，testable 728项中727通过，均仅剩已登记的 P3 ValueRoot 用例。
- P1阶段14个目标：四臂各N=3。基准/恢复全到达并通过；old顺序刀仅颜色顺序目标失败，young刀仅两条remset顺序目标失败。
- 位图、真实producer/consumer及混代selection都有产品SO故障臂；组合断线补丁通过基线承重点检查。使用相同测试ELF，完整差集和身份均已归档。

## 保留结果

托管进程在P1完成后触发 #607 字段屏障守卫，SIGABRT，不记为整程通过。原managed runner的 #629 装置失败及OHOS runner因缺.git退出128均保留。带已知红的合并许可需独立Review后另行绑定提交裁定，本PR不自授豁免。

[完整报告与证据索引](https://github.com/cjcj-dev/cangjie-runtime/blob/sym/606-implement-r5673376405/delivery-r2/REPORT.md)
