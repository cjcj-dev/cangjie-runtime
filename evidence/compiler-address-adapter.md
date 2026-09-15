LANE=sym_cangjie_runtime_608_implement_r5676392826
ROLE=implement
PROGRESS=WIP
P01 核心 runtime 744ed41bf 已两构型编译成功，LLVM load SHR/store SHL 已在候选树修改，联合构建启动中。
按调用链核到新增承重面：/root/cj_build/llvm_rebase_wt/sym_cangjie_runtime_608_implement_r5676392826/llvm/lib/IR/SafepointIRVerifier.cpp:1000 uncolorIfGCPtr 对 AS1 参数发 llvm.ptrmask(..., low48)，调用者包括 CJBarrierLowering.cpp:645/782 的 slot 地址、:1216/1229/1250/1251 的其他 lowering。它不只是本包正文点名的 read fastpath 槽值解码；直接改为 SHR 会把已是 uncolored 地址的 slot/derived pointer 再移位。
请确认该适配器归属：A 本包连同 verifier 做 AS1 uncolored 地址契约清理（删除冗余 ptrmask，更新 verifier 对已合法 plain 地址的边界），B 作为 Cangjie base/derived provenance 基础设施保留该层并登记事实+ZGC 锚，由后续编译器包处理。当前未修改该适配器，不借“等价”将它判为形态对齐。继续处理其余已授权项。
