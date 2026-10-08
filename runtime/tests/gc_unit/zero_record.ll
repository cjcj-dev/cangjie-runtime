; Actual paired LLVM producer input: empty metadata and nonempty return controls.
source_filename = "zero_record.cj"
declare void @record_callee() "gc-leaf-function"
declare token @llvm.experimental.gc.statepoint.p0f_isVoidf(i64, i32, void ()*, i32, i32, ...)

define void @zero_record() #0 gc "cangjie" {
  %space = alloca [24 x i8], align 8
  %p = getelementptr [24 x i8], [24 x i8]* %space, i64 0, i64 0
  store volatile i8 1, i8* %p
  ret void
}
define i8 addrspace(1)* @root_record(i8 addrspace(1)* %p) gc "cangjie" !dbg !4 {
  ret i8 addrspace(1)* %p, !dbg !8
}
define void @zero_root_record() gc "cangjie" !dbg !5 {
  ret void, !dbg !9
}
define void @line_record() gc "cangjie" !dbg !11 {
  %sp = call token (i64, i32, void ()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidf(i64 0, i32 0, void ()* @record_callee, i32 0, i32 0, i32 0, i32 0), !dbg !12
  ret void, !dbg !12
}
attributes #0 = { noinline "cj_fast_call" "leaf-function" "frame-pointer"="all" }
!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 1, !"Cangjie_PACKAGE_ID", !"zero_record"}
!1 = !{i32 2, !"Debug Info Version", i32 3}
!2 = !{i32 2, !"Dwarf Version", i32 4}
!llvm.dbg.cu = !{!3}
!3 = distinct !DICompileUnit(language: DW_LANG_C, file: !6, producer: "zero-record-product-test", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!4 = distinct !DISubprogram(name: "root_record", scope: !6, file: !6, line: 37, type: !7, scopeLine: 37, spFlags: DISPFlagDefinition, unit: !3)
!5 = distinct !DISubprogram(name: "zero_root_record", scope: !6, file: !6, line: 41, type: !7, scopeLine: 41, spFlags: DISPFlagDefinition, unit: !3)
!6 = !DIFile(filename: "zero_record.cj", directory: "/product-test")
!7 = !DISubroutineType(types: !10)
!8 = !DILocation(line: 37, column: 1, scope: !4)
!9 = !DILocation(line: 41, column: 1, scope: !5)
!10 = !{}

!11 = distinct !DISubprogram(name: "line_record", scope: !6, file: !6, line: 37, type: !7, scopeLine: 37, spFlags: DISPFlagDefinition, unit: !3)
!12 = !DILocation(line: 37, column: 1, scope: !11)
