; Paired with LLVM #100: descriptors and return polls are compiler output.
; A native observer enters the product safepoint path while these frames live.
@metadata_depth = global i32 0
declare void @observe_watermark(i32)
declare token @llvm.experimental.gc.statepoint.p0f_isVoidi32f(i64, i32, void (i32)*, i32, i32, ...)

define void @poll_chain(i32 %depth) #1 gc "cangjie" {
  %done = icmp eq i32 %depth, 0
  br i1 %done, label %observe, label %recurse
recurse:
  %next = sub i32 %depth, 1
  %sp = call token (i64, i32, void (i32)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidi32f(i64 0, i32 0, void (i32)* @poll_chain, i32 1, i32 0, i32 %next, i32 0, i32 0)
  store volatile i32 %depth, i32* @metadata_depth
  br label %exit
observe:
  %observe_sp = call token (i64, i32, void (i32)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidi32f(i64 0, i32 0, void (i32)* @observe_watermark, i32 1, i32 0, i32 1, i32 0, i32 0)
  br label %exit
exit:
  ret void
}

define void @no_poll_chain(i32 %depth) #0 gc "cangjie" {
  %done = icmp eq i32 %depth, 0
  br i1 %done, label %observe, label %recurse
recurse:
  %next = sub i32 %depth, 1
  %sp = call token (i64, i32, void (i32)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidi32f(i64 0, i32 0, void (i32)* @no_poll_chain, i32 1, i32 0, i32 %next, i32 0, i32 0)
  store volatile i32 %depth, i32* @metadata_depth
  br label %exit
observe:
  %observe_sp = call token (i64, i32, void (i32)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidi32f(i64 0, i32 0, void (i32)* @observe_watermark, i32 1, i32 0, i32 0, i32 0, i32 0)
  br label %exit
exit:
  ret void
}

define void @mixed_chain(i32 %depth) #0 gc "cangjie" {
  %done = icmp eq i32 %depth, 0
  br i1 %done, label %observe, label %recurse
recurse:
  %next = sub i32 %depth, 1
  %sp = call token (i64, i32, void (i32)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidi32f(i64 0, i32 0, void (i32)* @mixed_chain, i32 1, i32 0, i32 %next, i32 0, i32 0)
  store volatile i32 %depth, i32* @metadata_depth
  br label %exit
observe:
  %observe_sp = call token (i64, i32, void (i32)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidi32f(i64 0, i32 0, void (i32)* @poll_chain, i32 1, i32 0, i32 3, i32 0, i32 0)
  br label %exit
exit:
  ret void
}

attributes #0 = { noinline "frame-pointer"="all" "cj_fast_call" "disable-tail-calls"="true" }
attributes #1 = { noinline "frame-pointer"="all" "disable-tail-calls"="true" }
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"Cangjie_PACKAGE_ID", !"return_poll_metadata"}
