待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

坐标基于 308082dfda828e7b9227f928bbe7d6ff01d4e18c。仅离线配方，不证明Apple产品或完整AArch64/CFG。

生产/消费格式：publish_runtime_output.py:25,94-109,126,141-143 的canonical JSON身份以及config/hashes元数据；RuntimeOutputLayout.cmake:67,79-84的两份路径记录。离线调用真实generated_inputs/canonical_json纯读函数，从byte mirror形成格式，不调用publish/configure/外部工具。JSON不解释路径。所有formed .a/.dylib/.o/.obj按实体复制并hash；不依赖路径记录穷举安装副本/子target/发布库。声明路径缺失仍拒删留原树。MISSING仅标未形成的期望种类。

实际类别来源：build_cjthread.sh:64,67 install；Base/CMakeLists.txt:25与UnwindStack/CMakeLists.txt:28 STATIC；RuntimeOutputLayout.cmake:18-19 staging lib/ar；publish_runtime_output.py:132-143发布副本。旧default-configure.log:1813-1814证明install档案已形成，镜像只模拟字节，不声称新编译产物。

单批18项，各一次；首非目标异常停止，不改源重跑。执行前冻结本文件、源码、生产来源及hash。
1. publisher canonical JSON和配套元数据保全。
2. install/staging/Base/UnwindStack/发布类别逐对象hash。
3. 声明formed CJThread缺失，精确拒删留树。
4. 同格式CJThread恢复。
5. 声明formed link脚本缺失，精确拒删留树。
6. 同格式link恢复。
7. 子target archive复制失败，原实体保留。
8. 首arm失败，后两arm独立保全，first_error不变。
9. caller/callee合法LR恢复及strip结果。
10. R3原静态x0 ADD覆盖反例，目标返回值拒绝。
11. R3原静态x30 ADD覆盖反例，目标LR拒绝。
12. W0 MOV使X0标签失效。
13. W30 MOV使X30标签失效。
14. MOV覆盖地址base后不能沿旧slot接受LR。
15. 非写入CMP不破坏有效标签。
16. 合法SP SUB/ADD、存取、LR恢复和strip返回阳性。
17. unsupported控制流明确INVALID。
18. 调用出口消费checker false，定向拒绝。

每个LR项穿run.py实际instruction_checks→function_block→lr_flow（AST只取实际定义，避免导入执行产品驱动）；false项瞬态替换返回值只证明调用出口。保全项穿recipe.preserve_arms→preserve_then_delete→hash/copy/delete。总rc0包含目标拒绝被捕获，不伪称产品红臂进程rc非0。装置改动不改producer/consumer/cut或产品指令。工具实体绑定/cache域旧证据仅限定未改函数/原输入沿用；保全/LR旧通过不沿用。本批未覆盖真实Apple实体/cache命令、新objdump/CPU/green-cut-restore，继续hold。

kkk2 box+ulimit -c 0；增量≤256MiB，起跑avail≥26GiB、低于24GiB停止；不调用cache/compiler/configure/build/GDB/native/GHA。不push，不merge，不动main。终态TRIAGED/next Triage。
