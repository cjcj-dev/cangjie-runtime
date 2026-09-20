LANE=sym_cangjie_runtime_471_implement_r5747758651
补充上一问 sym_cangjie_runtime_471_implement_r5747758651-20260920T052323Z.md：testable 单元编译失败由我自己的调用遗漏 MRT_TESTABLE_INTERNALS=1 环境造成。run_standalone.sh:254-255 依此设主 ELF 头宏；产品 testable SO 不会自动设置它。ZCrossVM.hpp:68 已有 friend RelocationReceiptTestAccess。撤回“范围外测试缺陷”以及修复请求，不应开 issue；不改该测试。我将按正确配方重跑。old ExemptFromRegions 实际选集链的裁定请求仍有效。
