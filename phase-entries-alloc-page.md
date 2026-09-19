问题：entry_cut_check 要求切刀落 PHASE_ENTRIES，本包承重点是 Heap::alloc_page insert（zHeap.cpp:519，对应 ZGC zHeap.cpp:253-257）。清单无 alloc_page。

方案A：把 Heap::alloc_page 登记进 /root/cj_build/ops/coord/PHASE_ENTRIES.txt，切刀仍只删 insert 行。
方案B：额外在清单里已有函数上切一刀（与本包机制无关）。

倾向A：切的就是产品发布点。请裁。
涉及：zHeap.cpp:513-521 · PHASE_ENTRIES.txt
