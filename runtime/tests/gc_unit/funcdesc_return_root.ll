; Real managed input for #1266. Paired cjcj-llvm#101 llc must emit descriptors
; and return maps for both the call-bearing control and the legacy leaf input.
declare void @callee() "gc-leaf-function"

define i8 addrspace(1)* @slot_neighbor(i8 addrspace(1)* %object) gc "cangjie" {
  call void @callee()
  ret i8 addrspace(1)* %object
}
define void @neighbor_end() { ret void }

define i8 addrspace(1)* @slot_leaf(i8 addrspace(1)* %object) #0 gc "cangjie" {
  ret i8 addrspace(1)* %object
}
define void @leaf_end() { ret void }
attributes #0 = { "leaf-function" }
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"Cangjie_PACKAGE_ID", !"funcdesc_return_root"}
