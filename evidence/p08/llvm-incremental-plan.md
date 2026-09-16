LANE=sym_cangjie_runtime_614_implement_r5687433872
ROLE=implement
PROGRESS=WIP

# P08 LLVM 单 TU 增量方案（待主控容量/执行回执）

受影响target：LLVMCodeGen/CJBarrierLowering.cpp.o → 私有libLLVMCodeGen.a → 私有bin/llc；现候选4bb0c3ab7187c0e9fef3ce3e5378003aa15edcf9相对1a014519只改该产品TU及新增lit输入。不复制源码树、SDK、整build，不写原build/source/archive/ELF。

只读基座kkk2:/root/sym_cangjie_runtime_608_implement_r5683164869/abi-final-green：源码071fd4b2ec5c90c9bc09849b8fb6e80b789b5f4d，与1a014519产品tree一致（主控已裁）；实际回读CJBarrierLowering.cpp SHA384006c43e210d7cc667996f82b6c5b1689a04583b3b1407f0ae9b2e4d530764。llc 51,830,136B / b71a256f4db199ffcb452e1ed1ec5b12b96544058b0d77c286293b252443ad30；libLLVMCodeGen.a 25,929,614B / f1bcf4836823f882750032a981038062e5280e670afa5e84e067965cae144337；旧TU对象217,736B / 846fae07d33bdcf52e774da9d329867d4ccaf9a7accefd4937fa22bc4f9ce5c1。完整命令与SHA读证在llvm-plan-inputs.json；ninja -t commands -s两target均rc0。

执行脚本llvm-incremental-build.py只做：
1. 回核只读基座上述输入哈希；读Ninja已保存配方，不调用ninja build原树。
2. 单TU源码上传本棒/root/<lane>-llvm/inputs/<arm>.cpp，输出独立/root/<lane>-llvm/<arm>/，沿原编译argv/ccache；将失效的abi-canonical包含路径映射到真实只读abi-final-green，把-o/-MF与-c输入定向本棒目录；原对象与生成头不动。
3. 只复制25.9MB静态库到本棒目录，用ar rcs替换唯一CJBarrierLowering.cpp.o成员；先ar t核唯一，不增同名副本。
4. 原llc链接argv只替换libLLVMCodeGen.a及-o，其他库和生成头只读；所有实际link输入在链接前SHA、输出形成后立即SHA；记录真实overlay源码与其他基座输入血缘。
5. 不装入共享SDK；用本棒llc绝对路径运行lit和真实ABI消费者。基座FileCheck可只读复用（该target未改）。后续联合运行使用实际新runtime两SO及#48/#607合格输入，不借旧结果。

空间估算而非实测：每臂持久新增源码/dep/obj约1MB+archive25.9MB+llc51.8MB+日志/身份<5MB，约84MB；链接临时按再加一个llc预算约52MB，单臂峰值约136MB，预留256MiB/臂。3臂独立路径持久约252MB，两构建槽同时链接额外约104MB，总预算<512MiB。该估算在启动前按实际stat与df重核，空间不足不启动。单TU本身不可拆并行；不同臂在共享2构建槽内并行，链接按各自真实输入执行。runtime/tests走共享3测试槽及windows核域。

若P04需要同一LLVMCodeGen TU的组合候选，可由主控指定唯一组合构建owner，按逐hunk先合P04容量helper与P08其它函数，再对组合CPP执行同一单TU方案；不能两棒写原成功树，也不能以未组合源码的二进制代表联合候选。P08不改P04拥有的emitReservedHeapSlot/kCjHeapRangeCap。

容量新观测：本棒runtime build01双构型总370MB，编译完成/链接rc2（primitive模板可见性，已修复保存）；之后box df再次显示available=0，rc0。尚未启动任何LLVM构建或第二runtime批次。请确认本方案及新的有界容量窗口/组合构建owner。
