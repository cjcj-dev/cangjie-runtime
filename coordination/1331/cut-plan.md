待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
# 1331 切刀计划（尚未执行）

1. ZValue header-only：恢复基线 `_count` 和 next 读缓存；同一产品头文件本体编译，新 ELF 只应使 iterator_observes_current_storage_count 红。按协议 §2.5 模板例外记录，不冒充 SO 刀。
2. ZValue allocation：恢复 `_block_count` 分路（其余不变）；只使 storage_reuses_existing_block_after_count_change 红。
3. PageTable producer：构造 `_map(ZAddressOffsetMax)`，预期 page_table_non_power_of_two_extent 的 slots==expected 红。由于构造已移至 cpp，候选新增行刀单列。
4. PageTable consumer：恢复 count 按页计数，预期 count 目标断言红；另在 serial next 的产品调用处断开 granule iterator，验证真实页面结果进入断言。
5. 相位入口：基线 zHeap.cpp:512 page_table().insert(page) 断线。须避免影响堆初始化掩盖目标；需在合法页类型条件上构造定向输入后再选择具体刀，禁止无条件入口失败。

所有刀尚未执行；本文件不是验收证据。
