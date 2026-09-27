待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
LANE=sym_cangjie_runtime_1215_implement_r5855214347
已按前两问裁决实现 GC/栈指针位置表分离，候选 d6277cd1d8072e579c823c2e21ce8728c2fa04ef，两构型 rc=0，单元并发运行中。
上一问中的装置坐标尚未答复：请提供 LLVM#87 合入 aa4171e6ac97e8b90e50fb60e6dda0bfd755be66 后全量重编的 compiler/染色 std tuple 及输入负载路径，用于 #1215 强制的真实输入守卫预演和 Opus v3 同装置返回屏障计数。旧 stage2 857d854a 明确具有普通调用点寄存器根，不能用旧 ABI 完成守卫阴性验收。
若尚未完成重编，请明确本棒是否仅实现与 unit/DIFF 后交 WIP 等配对装置。继续做 unit、切刀、OHOS、DIFF，不修改共享 SDK，不把旧 ABI fatal 算通过。
PROGRESS=WIP
