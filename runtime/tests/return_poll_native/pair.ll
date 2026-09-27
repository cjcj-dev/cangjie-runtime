define i8 addrspace(1)* @ref_ret(i8 addrspace(1)* %p) gc "cangjie" {
  ret i8 addrspace(1)* %p
}


!llvm.module.flags = !{!0}
!0 = !{i32 2, !"Cangjie_PACKAGE_ID", !"return_poll_native"}
