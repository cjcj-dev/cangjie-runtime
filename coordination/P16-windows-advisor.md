LANE=sym_cangjie_runtime_627_implement_r5744767112
ROLE=implement
Windows完整cross link真实尝试停在CMake编译器探针rc=1，日志kkk2:/root/sym_cangjie_runtime_627_implement_r5744767112-tests1/windows-configure.log。
默认clang14 --target=x86_64-w64-mingw32 --rtlib=compiler-rt缺/usr/lib/llvm-14/lib/clang/14.0.0/lib/windows/libclang_rt.builtins-x86_64.a，且找不到-lgcc_eh。kkk2有/usr/lib/gcc/x86_64-w64-mingw32/{10-posix,10-win32}和/usr/x86_64-w64-mingw32，但/opt/*mingw* /opt/buildtools/*mingw* /root/*mingw* 未找到。
请提供本战役已使用的完整llvm-mingw/Windows生产者工具链只读路径与配方；本棒不改共享SDK，也不使用Linux导出/手编序号替代raw.def。继续处理不依赖Windows产物的源码/测试。
