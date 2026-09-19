待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标：冻结325d6ad73f99cc463a804aedd39595af05791c9a；候选36d06fe5c（后续提交以报告头为准）。本表是实现记账，不是独立审查结论。

| ZGC 锚（/root/cj_build/reference/jdk/src/hotspot/share/gc/z/） | 本包实现 | 验证方法/剩余 |
|---|---|---|
| zVerify.cpp:63-110；zBarrier.inline.hpp:320；zIterator.inline.hpp:43；zUncoloredRoot.inline.hpp:36 | z_verify_safepoints_are_blocked，三个debug漏斗 | Debug真实输入预演待测试槽；不得宣称已验证正常输入边界 |
| zVerify.cpp:121-199 | 文件内静态 oop/root/old/possibly_weak 函数；页区间判定；预加载barrier不自愈 | ZVerify精确12项N3；旧公开Object/Oop删除 |
| zVerify.cpp:395-424,467-503 | object/field闭包、brokenObject在AfterMark保证、abort最前、Heap迭代包装 | 图对象测试保留；所有对象承重面断线尚未齐备 |
| zVerify.cpp:531-739 | Before/After OopClosure保存from/to；前查页级inactive remset；后经表遍历+generation.remap_object | BeforeRelocationRejectsMissingRememberedField绿；新闭包重建双构型rc0；最终差分待跑 |
| zRelocate.cpp:877-886 | CompactRegion重置top后、交换remset前检查inactive位图 | 新页级CHECK；完整生产入口断线待补 |
| zVerify.cpp:741-761 | AfterRelocation入口to_age old守卫 | 源码对照；最终差分待跑 |
| zMark.cpp:1022-1035 | ZMark成员verify_all_stacks_empty start/end；协调者TryTerminateFlush遍历worker本地栈 | MarkingStacks.VerifyEmpty旧接口/调用删除；成员测试已迁移 |
| zStoreBarrierBuffer.cpp is_in | static遍历mutator缓冲并remap | 删除旧MutatorManager原始比较包装；最终测试待跑 |
| zVerify.cpp:489-495 | VM_ZOperation::pause → BeforeZOperation → RootsIteratorStrongColored::Apply | 两处基线产品行分别切断，N3精确红/恢复；entry_cut_check rc0 |

## 已获裁决的保留项（不是清零）
| 保留 | 事实/消费者 | ZGC锚与归属 | advisor outbox 文件尾 |
|---|---|---|---|
| Heap/shared/stringdedup | 活跃字符串去重库 | gc/shared/stringdedup；合法基础设施目录 | 194134Z |
| Heap/Allocator/{Allocator,RegionSpace,CartesianTree,HeapFiller,AggregateAllocator等} | Heap CMake仍列4个活跃cpp；Runtime初始化、页管理调用 | zPageAllocator/zObjectAllocator；#727/#730；不搬目录伪装删除 | 194134Z |
| VM_ZOperation以外旧Preforward的BeforeZOperation、VerifyRelocatedPage四个调用 | 在飞页生命周期接续 | zRelocate.cpp:1005-1008；#727统一 | 195839Z、201036Z |
| threads_start_processing内部finish_processing | Cangjie编译器无return statepoint，需要完成水位处理 | zVerify.cpp:442-465；#498基础设施差异 | 195839Z |
| Heap::_test_page_allocator/bind_test_page_allocator | fixture构造注入及绑定寿命 | zHeap.cpp:500-520；#720依赖#727 | 205808Z |
| LargeArrayInitTestHooks/MRT_GC_UNIT_TESTS及共享MarkClosureObserver | test_segmented_array_init.cpp全部30项原样保留；MArray分段清零及根访问原接线恢复 | zObjArrayAllocator.cpp:46-90；#730同批移植/删除旧钩子 | 210816Z、211628Z |
| windows_x86_64_exports.def旧符号及重复ordinal | 实际MinGW完整链接因LocalDeque.h:15 sys/mman.h止于编译，没有raw.def | #727修Windows producer；P16规范表冻结仍未闭合，禁止手工选号 | 201353Z |

advisor绝对前缀：`/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_627_implement_r5744767112-20260919T`，后接表中尾及`.md`。

## 删除与测试变更来源
- `P16-deletion-differential.tsv` 是相同git grep尺对冻结/HEAD的差分，rc与行数分开。基线已有为0的名字不能作为本包删除成果。
- `.def`还含g_installDomain/g_forwardRace旧导出，非产品定义；规范化依赖上表Windows完整link，未声称全树零命中。
- `P16-retained-hooks.txt`逐行列出剩余宏，全属分段数组及其共享observer；不添加豁免开关。
- `P16-test-name-difference.tsv`机械枚举GC_TEST/GC_OTHER_VM_TEST名字集合差；条件编译注册以最终runner清单为准。
- #711明确D档套件删除及RegionAge期望按zPageAge.hpp:30-46修改。
- allocation-stall五项→#730；StorageObserver两项→#727；Y2y无产品Push链→203416Z裁决删除；纯receipt/closure计数五项→201036Z。
- ReferenceProcessor CAS案例改为真实liveness回调更新referent并检查最终状态，不能外推为强制中途CAS竞争。
- PackageInit观察来自测试侧dlclose平台边界及真实析构函数，保留卸载结果/互斥/根资格断言；纯CompletePause观测接口案例删除。
- 标记数组/young weak closure用例按205557Z迁到真实mark_start/concurrent_mark相位，读取实际对象图mark位，未弱化图可达性断言；这类相位单测不代替RequestGC全链证据。
