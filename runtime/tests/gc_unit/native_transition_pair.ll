@native_depth = global i32 0
declare void @native_observe()
declare token @llvm.experimental.gc.statepoint.p0f_isVoidi32f(i64, i32, void (i32)*, i32, i32, ...)
define void @native_chain(i32 %depth) #0 gc "cangjie" {
 %done = icmp eq i32 %depth, 0
 br i1 %done, label %native, label %recurse
recurse:
 %next = sub i32 %depth, 1
 %sp = call token (i64, i32, void (i32)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidi32f(i64 0, i32 0, void (i32)* @native_chain, i32 1, i32 0, i32 %next, i32 0, i32 0)
 store volatile i32 %depth, i32* @native_depth
 br label %exit
native:
 call void asm sideeffect "subq $$16, %rsp; movq $$0, (%rsp); leaq native_arm(%rip), %rax; movq %rax, 8(%rsp); callq CJ_MCC_C2NStub", "~{rax},~{rcx},~{rdx},~{rdi},~{rsi},~{r8},~{r9},~{r10},~{r11},~{memory},~{dirflag},~{fpsr},~{flags}"()
 call void @native_observe()
 br label %exit
exit:
 ret void
}
attributes #0 = { noinline "frame-pointer"="all" "disable-tail-calls"="true" }
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"Cangjie_PACKAGE_ID", !"native_frame_pair"}
