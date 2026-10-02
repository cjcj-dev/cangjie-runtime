待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`

# #1478 prefix 平台接线独审书（N0）

坐标基于 d164ba01a6cc7730746f3d607b50b5006c80f5ed，产品源码与3d9c002e0adf519578fe54d97fe08c117de61967一致。候选是本分支后续测试提交，不push、不编译、不运行、不激活GHA。本书不是实现者自审或平台验收结论。原Reject与ABI/tuple/Apple/A1/#135/stage33/release hold均保持。

## 真实生产消费链

JDK固定5b2d6991a1279d375f9a3c00c7bcd0bbcc7081d6：runtime/frame.cpp:232调用CodeCache::find_blob；code/codeCache.cpp:750-759先找所属heap再heap->find_blob；memory/heap.cpp:468-478经find_block_for定位已使用块，最后blob_contains检查；code/codeBlob.hpp:285限定blob所属范围。Cangjie采用平台注册镜像范围+数据descriptor，不复制HotSpot指令读取模型。

平台loader装载fixture → fixture getter只准备记录/页权限 → 产品ElfUnloadQuiescence::LinkImage → 平台范围采集 → ImageDirectory::Rebuild归一化并发布 → PrefixResult读真实产品range/逐字节所属image → 原Resolve入口 → 产品FrameInfo::ResolveProcInfo（FrameInfo.cpp:67-87）→ 非Apple MFuncDesc::GetFuncDesc(startPC)（MFuncdesc.inline.h:64-82）→ 产品返回bool进入METADATA_PREFIX_TARGET和最终rc。测试工程不编runtime/src，不改ImageMap，不增加产品观察钩子。

## 合法输入及适用边界

Linux ARM64：沿用a2_contiguous.ld（left R at0x400000 / right RX at0x401000）与a2_prefix_hole.py（精确memsz：左止于0x400fff，右始于0x401001，prefix0x400ffe宽4）。使用现有metadata_owner_records.cpp的实际data descriptor；getter沿原PackageInitContinuousPC做法仅把真实偏移写入fixture，再恢复R/RX权限，之后才注册。max-page-size=4096及getter实测page==4096是明确配方边界，不承诺64KB宿主页适用；不足返回输入INVALID。内部gap页仍可读取，产品登记按PT_LOAD精确memsz保gap，不按OS页扩张。outside沿原section-start=.a2_boundary=0x400000输入：可执行PC本身在owner，prefix四字节不属owner；测试不读取这些prefix字节。ordinary/continuous/hole分别验证实际offset指向该fixture自己的descriptor。真实ARM64链接/加载尚未取得，须平台执行验证而非本机语法解析替代。

Windows x64：普通prefix沿原PE .gcmeta记录。新增cj_metadata_contiguous.dll沿同fixture TU，在独立.a2prefix节用65536对齐，prefix始于页边界前2字节，PC在后一页偏移2。getter GetSystemInfo要求4KB页且首地址页对齐，VirtualProtect仅把前页降为READONLY、后页保持EXECUTE_READ；没有改PE header或字节归属。产品在此后用VirtualQuery实际采集MEM_COMMIT region；资格断言必须看到两侧相邻且exec不同的同image登记range，以及实际offset/descriptor。失败=INVALID，不算产品目标红。原cross-owner/absent descriptor/data PC负输入继续执行。没有原平台完整prefix全缺fixture，不能虚构已有Windows outside输入。

Windows两字节内部registered hole按advisor232527Z裁定明确不适用当前生产路径，独审仍须复核。Microsoft VirtualQuery契约：https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery 的Parameters/Remarks规定地址页向下取整、连续同状态/同分配/同保护页组成region；MEMORY_BASIC_INFORMATION：https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-memory_basic_information 的BaseAddress/RegionSize定义页region。VirtualProtect：https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect 的Remarks规定只改变committed页，Parameters说明跨页操作影响整页。结合产品ElfUnloadQuiescence.cpp:556-568只按RegionSize收集、Rebuild只分既有端点、prefix固定4字节，推导页级孔不可能产生首尾所属同image而中间两字节不属的形态。PAGE_NOACCESS仍MEM_COMMIT，保护故障不是所有权孔。stdout与build-result显式写APPLICABILITY_NOT_PRODUCIBLE_BY_CURRENT_WINDOWS_REGISTRATION，不能写PASS、已运行或已闭合因果臂。

## 实际CMake依赖与执行入口

metadata-code-shape只编metadata_code_shape.cpp，链接cangjie-runtime/boundscheck；metadata只编main.cpp/test_empty_stackmap.cpp及Windows现有windows_input.S。owner/foreign/contiguous只编metadata_owner_records.cpp。shape依赖owner/foreign/contiguous，Linux contiguous POST_BUILD调用原hole生成器得到第四个fixture；Windows无PHDR生成器。default仍只构建shape依赖闭包，testable同时构建metadata；没有gc_unit_main/其它套件依赖。

四构型正式入口不变：python3 runtime/tests/gc_unit/metadata/build.py a2 --config default 或testable；内部CMake使用Ninja，build --parallel cpu_count，无taskset。shape bundle中的实体验证产物与runtime/fixtures哈希后启动一次。N0本轮从未运行这些命令。

## 有界次数（配方推算，非测量）

四构型候选原潜在ELF启动24：Linux testable metadata监督器8次（abort1+名单3+unknown1+缺exe1+timeout1+非目标1）；Windows testable12次（abort1+名单6+unknown1+缺exe1+timeout1+非目标1+缺DLL1）；shape四构型4次。24包含缺exe/缺DLL失败尝试，不能记24有效产品完成。

新增独立shape ELF启动0、metadata启动0，仍24潜在启动。新增真实Resolve调用Linux4×2+Windows2×2=12（原shape4×4=16，合计28），全部在已有四shape进程内。新Linux目标ordinary/continuous/outside/hole各2，Windowsordinary/continuous各2；hole适用性记录Windows2不是目标通过。新增fixture实体构建4（每构型一contiguous），Linuxhole派生2；新增平台加载6（Linux各contiguous/hole×2，Windowscontiguous×2）。全部执行N=0。

新增工具子进程：cmake help4+ninja commands4+llvm-readobj14（Linux4fixture×2、Windows3fixture×2）=22；另外原hole生成器2次（CMake POST_BUILD，计于fixture生成），不算产品ELF启动。原所有metadata supervisor校准与拒绝输入保留。失败不续跑取绿。

## workflow与首错保全

metadata-platform.yml逐字保持d164：workflow_dispatch要求candidate_sha==github.sha、sym/1478分支、a2_batch=linux-windows-candidate且resume_run为空/pac_resume=false，展开ubuntu-24.04-arm/default,testable与windows-2025/default,testable。旧prepare/arms/Apple capability不随这个选择启动；full/resume/PAC分路维持原条件，未改变权限或绕Apple403。权限contents:read；公共官方runner与架构断言保留；Windows仍MSYS2 CLANG64；sccache GHA启用，C/CXX/ASM及测试CXX/ASM launcher沿原配方，并行cpu_count。

run在启动前保存argv、cwd、command_sha256和STARTED，日志保原命令；结束保存rc/wall，TimeoutExpired=124/OSError=127并写原异常，不重试。失败编译/加载不是目标红；外层35分钟timeout仍写workflow-command.rc，always artifact保*.log/*.json/*.rc、runtime源码与构建树、a2-test-build全量（build.ninja/.ninja_log/objects/link.map/原失败输出）、bundle、linked-product。生成器源码哈希在configure前保存，ELF/fixture哈希在copy后检查前保存；candidate identity约束后续arm的同测试/fixture。首错如果发生在bundle前，仍由原always路径保全源、对象、Ninja命令与build log，不造替代成功原件。

## 后续真实平台运行与精确红（本轮未执行）

先由另一sol对本跨度与d164 workflow独立接线审查，尤其ARM64 PHDR合法装载、PE section/page布局与当前Windows适用性推导。主控再裁固定新head的四构型候选真实批，不能先push/GHA或借用原Linux/R1/OHOS/managed通过外推。配方实际完成时须读Ninja命令、ELF/DSO头、导入真实产品符号、每个INPUT/descriptor/target行与真实rc；INVALID早于target须保留并不得计通过。

之后若获单独切刀额度：Linux旧首尾判据刀落产品MFuncdesc.inline.h:73-77（3d9c已有产品行），仅把全字节资格恢复为首尾资格；同一candidate ELF及三个owner/foreign/contiguous/derived hole实体，只有产品SO改变。ordinary/continuous/outside与原四owner目标应保持，只有hole accepted从0变1、目标红；恢复SO须等于candidate、刀SO不同。Windows该内部hole因果刀不适用，不安排虚构敏感性；需要另行已授权的适用拒绝面刀时沿现code-only/descriptor-owner既有刀，仍不得本轮启动full。报告需逐平台实际rc/哈希/输入qualification/执行target，不提前写已精确红。本轮不新增切刀arm或改变workflow触发。
