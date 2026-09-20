LANE=sym_cangjie_runtime_739_implement_r5747765432
冻结 b6d62daa8f3557c8a9effbfa4709a4744e315497。
实读发现不变量1“所有权在 GC driver 侧”若包括 satisfy(true)，与 ZGC 不符：reference/jdk/src/hotspot/share/gc/z/zPageAllocator.cpp:2164,2167-2189 是 free_pages 后在 allocator lock 下满足成功请求，我方 runtime/src/Heap/z/zPageAllocator.cpp:710 同样在 ReturnRetiredPageMemory 中满足。拟仅将失败答复与 restart 移到 driver 周期末，保留 allocator 容量归还 satisfy(true)，请裁定。
退出路径另一个实际形态差：我方 zDriver.cpp:80 每周期 ZAbort::reset()；参考 zAbort.cpp 无 reset，driver 在 driver lock 下 abortpoint。该 reset 能清除 stop() 已置位的 abort，拟随本 issue 去掉每周期 reset、仅在初始化启动前 reset（本地支持 runtime 重启，HotSpot 不支持）。另外请求确认允许 shutdown 在 driver 退出前排空答复 false 作为本地 runtime 退出适配（ZGC allocator 无 shutdown 排空对应物）。
原 medium 用例原 ELF+原 SO 单次 gdb 到 Fini入口并正常结束；证据 kkk2:/root/sym_cangjie_runtime_739_implement_r5747765432-evidence/baseline-gdb.log。尚未定位原 rc124，计划按源码构造停止与 driver 周期交错取栈，不反复压力碰运气。
