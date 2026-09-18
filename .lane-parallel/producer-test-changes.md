# #700 测试增删补记

| 项目 | 动作 | 原因与归属 | 验证 |
|---|---|---|---|
| product_colour_producer_manifest.tsv 的 stale_load_bad 行 | 删除 | #700 删除无消费者的 ColourStaleLoadBad；保留条目将检验不存在的机制 | producer-manifest-repair.json：f49bd78712f5 基线仅定义命中，7bfbf781fa23 产品符号无命中，store_good 为在场阳性对照 |
| run_standalone.sh EXPECTED_PTRCOLOUR_PRODUCERS 的 stale_load_bad | 删除名单项 | 与同一机制删除同步；其余四个真实 producer 名称、anchor/family/计数检查保留 | 源码差分；bash -n |
| test_native_root_current.cpp 的 Mutator.inline.h | 显式包含 | 头依赖解环后补 EnterSaferegion(bool) 的实际内联定义可见性 | 源码差分，等待统一构建/运行 |

测试名集合无新增/删除；此处删除的是一个已退休机制的 manifest 行，未改其余测试断言。

7bfbf781fa23 三臂日志 kkk2:/root/diff_7bfbf781fa23/unit-{default,filler,testable}/run.log 报装置失败，套件尚未运行，不能记为候选独红为零。修复后仍待重跑。
