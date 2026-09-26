; LLVM #12 paired producer: the reference is live only in the return register.
; Do not hand-assemble the poll or the compressed return oop map in this test.
define i8 addrspace(1)* @ref_ret(i8 addrspace(1)* %p) gc "cangjie" {
  ret i8 addrspace(1)* %p
}
