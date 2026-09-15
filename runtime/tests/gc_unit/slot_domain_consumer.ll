; Consumer for segmented InHeap: lowered by candidate llc cj-barrier-lowering.
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)

define i8 addrspace(1)* @slot_domain_read(i8 addrspace(1)* %base,
                                        i8 addrspace(1)* addrspace(1)* %field) gc "cangjie" {
entry:
  %value = call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %base,
                                                 i8 addrspace(1)* addrspace(1)* %field)
  ret i8 addrspace(1)* %value
}

define void @slot_domain_write(i8 addrspace(1)* %val, i8 addrspace(1)* %base,
                               i8 addrspace(1)* addrspace(1)* %field) gc "cangjie" {
entry:
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %val, i8 addrspace(1)* %base,
                                 i8 addrspace(1)* addrspace(1)* %field)
  ret void
}
