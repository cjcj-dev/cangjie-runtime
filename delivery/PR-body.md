P1 将页分配存活资格改为 birth sequence，并新增每代 typed mark 入口及 offset consumer；删除 pinned 旧槽分配侧显式 mark。保留 P2/P3 未迁裸入口。

双构型构建 rc=0；定向 12 项绿/恢复通过，6 类故障臂目标转红。完整 default/filler 各 526/527，testable 716/717；剩余 ValueRoot 失败按 advisor 绑定 P3，未豁免。OHOS 构建 rc=0，runner 因归档树缺 Git 元数据退出 128。

部分分支的真实入口闭环证据仍不足，详见 delivery/REPORT.md，需独立审查，不构成合并许可。
