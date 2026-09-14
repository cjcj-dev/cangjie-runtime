LANE=sym_cangjie_runtime_571_implement_r5668109509
ROLE=implement
PROGRESS=WIP
Q62 真实输入预演发现守卫边界，不送审。
按前答要求对旧 gate-colored-560b 三 ELF 各 N=3 做只读 gdb，逐槽名单 kkk2:/root/sym_cangjie_runtime_571_implement_r5668109509-preview-final/REJECTED_ROOTS.md 与 rejected-roots.json，九发均在既有 SIGABRT 停止（gdb rc0不当负载通过）。finalizer/phase 各85个无色非空槽、segmented18个，同输入三发同集。
其中 finalizer/phase 各21个不是 .bss，而是 ELF .data.rel.ro 静态字面量根，target 也在 ELF，例如 finalizer：slot=0x555555817b50 `_CNbb17YEAR_OUT_OF_RANGEE`，word=0x555555818480，target type RawArray<UInt8>；`_CNan12SLASH_STRINGE` slot=0x55555581ad80 → word=0x55555581b590；调用栈全部 StaticRootTable::VisitRoots。它们不是堆对象，也不是 compiler phase<=8 plain store 后仍需 GC搬迁的根。
基线 EnumRefFieldRoot:116-118 在 mark-good快路已按 !Heap::IsHeapAddress(target) 直接跳过，slow path同样:144跳过；ReadStaticRef原本直接返回非堆静态对象指针。当前按前答添加“所有无色非空 NativeSlot 拒绝”的 CHECK 会拦到这些正常字面量（编译产物静态初始化，不是 runtime 自己 WriteStaticRef 的无色写）。此前“全部命中是 .bss compiler快路”被实测证伪，不会据此上线。
请求裁：是否把新守卫域限定为 Heap::IsHeapAddress(槽payload) && 无remap/marked（保留现有非堆字面量原样读取/根扫描跳过），并加通过真实注册与 ReadStaticRef/EnumAllCommonRoots 两入口的非堆只读静态根边界对照？或要求 compiler#1/#585 一并改只读字面量编码（该方向需另裁，当前不能宣称正常输入零误拦）。不自行放宽 LOADFC，也不新增side metadata。
独立工作已完成：主线 #579 fca1c8ce 合并，candidate edff141151919242f286b68f051707c4d8f7c781；双构型rc0，default526/testable704全套运行rc0，OHOS真实runner前验rc21；八臂5用例各N3，入口/两remap/两heal精确转红，绿恢复SO同sha。两发布实参gdb N3均等于真实CompactRegion forwarding-to（minor FollowOnly / major MarkAndFollow），证据 kkk2:/root/sym_cangjie_runtime_571_implement_r5668109509-post-evidence/。这些结果不替代守卫边界裁决。
