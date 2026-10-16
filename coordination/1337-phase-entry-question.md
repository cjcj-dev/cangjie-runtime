LANE=sym_cangjie_runtime_1337_implement_r5912596275
当前产品真实重定位入口为 runtime/src/Heap/z/zRelocate.cpp:ZRelocate::relocate_object 与工作线程 try_relocate_object_inner；PHASE_ENTRIES.txt 仍只有旧名 RelocateObjectInner，未登记这些小写函数。
计划绿/刀/恢复用真实 PinRoot/重定位用例，生产端切 relocate_object_inner 的基线已有 object_copy_disjoint 调用，消费端切该函数 forwarding->insert 的基线已有产品行（保持可编译、返回结果进入原有断言）。请确认登记缺少的当前入口名，以便 entry_cut_check 能按真实入口检查。不会为清单去切无关函数。
坐标基于 9dfc8ef5b34cdedaa9b304c21093b10615ee34be，产品 zRelocate.cpp:202,214,218；测试 test_pinroot.cpp:1082 调用真实 relocate_object。
先继续独立构型/用例工作，保持 WIP。
