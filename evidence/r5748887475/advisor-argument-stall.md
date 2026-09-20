AddCJArg跨分配反例受邻接分配停顿路径阻碍，先报告，不扩#739：
1. 32MiB堆，第二个ref-struct参数8MiB，预填dead large pages。真实MCC_ApplyCJStaticMethod→AddCJArg→NewObject→OOM GC之后仍申请失败（live少于堆容量），在隐式OOM异常未初始化断言退出；kkk2:/root/sym_cangjie_runtime_581_implement_r5748887475_stable/argument.log（不是目标红，未计有效）。
2. 改成small pages填已知容量缺口，第二参数8KiB（小页），保留三小页存活以确保relocation set非空。进程停在GC/分配等待，528/529并行用例已完成而该项仍在等；主动取gdb栈并停止本棒子进程（无重复碰运气）。kkk2:/root/sym_cangjie_runtime_581_implement_r5748887475_stable/default/unit/test-logs/000290-main.log、argument-blocked.gdb；该控制无效，不标PASS。
需要裁定一个不依赖allocator stall的新构造入口：能否用单独测试侧分配故障注入器，在真实MCC_ApplyCJStaticMethod的第二次MObject::NewObject调用前同步RequestGC，再转调原产品MObject::NewObject且不触碰其入参/结果？透明interposition不另编产品实现，不读写中间结果，所有臂同一个注入器及ELF；但它定义同名入口包装，按协议严格文字可能不合，故先问，未实现。
或者允许在现有诊断注册表新增一次性allocation safepoint测试钩子（不以计数判PASS），全部产品结果仍由原分配器产生；不擅自加临时探针。
独立继续注解/水位线红臂、稳定槽LIFO清理以及其余调用者。若主控有更符合约定的既有装置，请给路径。
