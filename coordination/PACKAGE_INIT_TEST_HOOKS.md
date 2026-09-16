# #646 原生测试接缝（待主控登记；#627清扫归属）
待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest 及现有诊断/测试钩登记，工具仓不在本棒写集。

头文件：runtime/src/Loader/PackageInitTest.h，仅 MRT_TESTABLE_INTERNALS 开启。
- bool MRT_PackageInitArmCompletePause(const void* package, const void* unit, uint32_t phase) noexcept
- bool MRT_PackageInitCompletePauseReached() noexcept
- void MRT_PackageInitReleaseCompletePause() noexcept
- bool MRT_PackageInitHasWaitingCaller(const void* package, const void* unit, uint32_t phase) noexcept

Arm 在启动目标 initializer 前调用，参数是真实 canonical package/code unit 地址及unit归属phase。Arm返回false表示已有未结束的暂停配置或参数无效；不改变cache状态。一次配置仅匹配该三元组。不同package/unit/phase Complete不被暂停。
Complete从真实产品入口进入后，在发布前查询精确token/owner身份，在graph短锁之外执行原生WaitqueuePark；只停当前逻辑CJThread。Reached表示已进入此等待点，Release使它继续执行原产品FinishToken/发布/通知。钩不写cache、不改phase、owner或终态，不替代Complete，也不拦stdlib其它unit。

所有导出与调用都受同一个编译期宏控制；默认构型无钩。Complete仍是noexcept但可park/GC，#48须走CJ_MCC_PackageInitComplete的现有保存寄存器桥，默认C convention/cj-runtime属性，不可标gc-leaf。

本棒测试：PackageInit.CompletePauseUsesLogicalWaitAndExactIdentity；通过未匹配unit正常完成、匹配unit停在发布前、第二CJThread不得Ready、期间真实GC、Release后Ready四段证明接缝。该native fixture不替代#48实际生成DLL的managed桥/字符串cache联合证明。

追加只读观察：HasWaitingCaller只在短graph锁下扫描真实Coordinator.waitingOn，并匹配三元组及Initializing状态；不写计数或状态。先Reached，再确认该查询true且第二调用未完成，然后GC，Release后两调用完成且查询false。测试同时检查错误unit/phase查询false作为边界对照。
