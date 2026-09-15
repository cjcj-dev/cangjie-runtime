LANE=sym_cangjie_runtime_608_implement_r5676392826
ROLE=implement
PROGRESS=WIP
已按 075948Z A 清除 LLVM uncolorIfGCPtr，且 LLVM 新 verifier 正例 rc=0、两条地址/值边界反例各 rc=134 命中目标诊断。
真实 std 输入预演被挡：kkk2:/root/sym_cangjie_runtime_608_implement_r5676392826/llvm/evidence/std-v2.rc=1；std-build.log 含多条 P01: GC addresses are plain; ptrmask cannot uncolor a field value，输入有 %uncolor.ptr = llvm.ptrmask(%1,281474976710655)、对 %gcread.ref 的重复 ptrmask、对 %virtualFPtr 的 ptrmask。尚未据此启用验收/宣称通过。
向生产端追核：/root/cj_build/cjcj/packages/codegen/src/IRBuilder.cj:1144/1148/1193/3447/4159/6060 仍调用 :1422 UncolorIfGCPtr；故现有隔离 target/bin/cjc 自身还生产旧 ptrmask，单改 llvm_rebase 不能消除。这是本次真实输入证伪的第三仓 ABI 依赖，不把 guard 放宽为接受旧 ptrmask，也不做兼容剥除层。
请补派 cjcj 前端权威绝对坐标/ref/SHA、隔离分支与重建入口，或提供已经清除此生产端、可直接用于本包联合验收的前端 ELF+源码血缘。原两仓坐标无该信息，我未改 /root/cj_build/cjcj。继续做不依赖第三仓的 runtime 单元与证据。
