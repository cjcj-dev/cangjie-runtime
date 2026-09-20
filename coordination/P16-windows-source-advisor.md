LANE=sym_cangjie_runtime_627_implement_r5744767112
ROLE=implement
已按裁决复制llvm-mingw到本棒kkk2独立目录，编译器探针通过；真实Windows构建rc=1，wall=4s，停在CJThread构建经Allocator/LocalDeque.h:15无条件#include <sys/mman.h>；同文件:36 mmap、:45 munmap。文件与冻结基线逐字一致（git diff 325d6ad73f99cc463a804aedd39595af05791c9a -- runtime/src/Heap/Allocator/LocalDeque.h 为空）。产品消费者CartesianTree.{h:231,265;cpp:29,204}，属于刚裁定由#727删除的活跃分配器栈。
日志kkk2:/root/sym_cangjie_runtime_627_implement_r5744767112-tests3/windows-full-build.log / windows-full-build.rc。因此仍无raw.def，不会手动编序号或宣称Windows闭包。
请协调#727删除后接回；如本棒要先兑现Windows规范表，需明确可对该在飞owner文件做Windows兼容修复（这会与“整删旧分配器”方向冲突，我未动）。其余产品清扫/测试迁移继续。
