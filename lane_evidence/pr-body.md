补齐对象分配器的上下文检查：`PerAge::retire_pages` 在清除共享页前检查 safepoint，外层按 age 调用；`fast_available` 在查询共享页前检查 mutator 身份。对应 ZGC `zObjectAllocator.cpp:196-202,220-227`，标题中 alloc_object 的定位已由 advisor 裁定以参考锚为准。

测试覆盖 young/old 直接退役、young/old mark_start 和 TLAB 分配入口的非法上下文，并为既有正常测试补齐真实暂停前置条件。双构型、切刀恢复与全套差分结果见本条实现报告（验证进行中）。

Refs #917
