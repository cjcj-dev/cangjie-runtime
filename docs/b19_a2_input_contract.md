# B19 A2：线程根与 value/export 输入合同

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
产品坐标基于 `fb814a743fd7518de2f547bb15e6893bee25bb8c`；返工基线 `4f9a173ed86a77cd72de1863277c75e6652f8379`。
本页记录实现合同及验证边界，独立审查负责最终判词。

## 授权与基础设施差异

裁定 `/root/cj_build/ops/advisor/outbox/sym_cangjie_runtime_596_implement_r5673746875-20260915T023905Z.md` 覆盖先前限制，允许在真实 handshake 修复 C1–C4 同槽输入。
`024158Z.md` 将 export 生产端的重复转发归入本包。

ZGC 的 `zStackWatermark.cpp:164–173,209–214` 从 `prev_head_color` / `prev_frame_color` 提供历史色，`zUncoloredRoot.inline.hpp:38–59` 在同槽按 resolve → function → heal 执行；`:62–69` 仅在旧色时 remap。
我方无 return statepoint，`runtime/src/Heap/z/zStackWatermark.hpp:18` 的 eager handshake 与 ZGC lazy 逐帧存在基础设施差异。当前不声称拥有 ZGC 帧历史色保存恢复，不给该面形态通过。历史色容器及 lazy 逐帧由 #498 统筹；本包不改该文件。

## producer → 保存 → consumer → 恢复

以下我方路径相对 `runtime/src/`，ZGC 路径相对 `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`。

| 面 | 生产与保存 | 真实消费与恢复 | ZGC 锚及输入资格 |
|---|---|---|---|
| C1 refVisitor | 栈对象字段真实内存，由对象类型遍历 `Mutator/Mutator.cpp:1058` 交给 refVisitor；`:1028` 取 RootSlot，分类用 PlainRootObject 不写槽 | `:1031` → PushHeapRoot `:889` 读取 observed，`:899` make_load_good，`:900` PublishThreadRoot，`:901` HealRoot；恢复在原字段 | `zStackWatermark.cpp:209–214`、`zUncoloredRoot.inline.hpp:38–59`；保留原槽携带的历史色到解析，不从分类得到 current 资格 |
| C2 ObjectRef | stack-map 保存槽或 nativeFrameRoots；后者 `Mutator.cpp:435–439` StorePlain，登记槽一直持有至 RemoveNativeFrameRoot；scanner 在 `zRootsIterator.cpp:268` 找实际槽 | `Mutator.cpp:1045` 传原 ObjectRef，进入同一 resolve → mark → heal；不再在 mark 前 StripRootObjectColour | 同 C1；plain 输入依赖已有 eager 保存/重映射合同，不把当前全局颜色假充历史色 |
| C3 invisible | `Mutator.h:507–512` StorePlain release；`Mutator.cpp:417–425` acquire 后传原 rawObject | `Mutator.cpp:1062` PushHeapRoot 原槽且 follow=false；解析和同槽恢复仍由 `:899–901` 处理 | `zStackWatermark.cpp:164–173`、`zUncoloredRoot.inline.hpp:79–80`；DontFollow 保留，原槽被读取后才可发布 current |
| C4 headerless | 调用约定提供 record 指针，record+0 是引用字段；不是对象头。`:1038/:1052` 选择 headerless 分支 | `Mutator.cpp:905–912` 将 record+0 本身作为 RootSlot，转同一屏障；不再 memcpy 到临时值后发布 | `zUncoloredRoot.inline.hpp:38–59`；String/无头值布局是基础设施差异，不免除同槽解析与恢复 |
| export 新值 | `zHeap.cpp:377` → `zRootsIterator.hpp:77` 保存 NativeSlot；`zRootsIterator.hpp:106` ReadStaticRef 返回 load-good 值 | `zHeap.cpp:390–402` CrossAccessBarrier 直接交 ResurrectExportObject；删除按页归属再次 ForwardObject | `zUncoloredRoot.inline.hpp:62–69`；GetExportObject 的屏障结果是 IncomingNew 的生产端资格，地址同时是别的 from-key 不改变它 |
| value 保存 | `zMark.hpp:452–474` 保存 object/stage/color/generation；Stage 按所属代的 relocation 色判断失效 | `zGeneration.cpp:805–818` IncomingNew 验证并保持对象身份；OverwritePrevious 从 page owner 的 table 解历史输入 | `zUncoloredRoot.hpp:46–49`、`zUncoloredRoot.inline.hpp:62–69`；必须同时知道输入身份，不能仅由页归属猜测 |
| value 消费恢复 | set/map 中每个 key/value 都保留上述容器状态 | `zGeneration.cpp:831–855` 遍历解析，构造 IncomingNew 后 swap；上层 minor/major 消费都读恢复后的结果 | 同上；同对象所属代翻色失效，不相关代翻色不能把 current 值当旧输入 |

## handshake 时序与 current 资格

`Mutator.cpp:330` 先 GcPhaseEnum 再完成 epoch ACK；`:940–976` DrainStackWatermark 持 MutatorLock，GC owner 必须在 saferegion 才消费。原保存槽在 `PushHeapRoot:891` 读取，observed 色直到 `:899` 解析时仍在；`:900` 才把 current 对象交给 mark，`:901` 写回原槽 plain，之后线程才恢复。
`GCPhasePreForward:1116` 是独立 REMAP 阶段；它不是此次历史色根的修复证明，也不替代 ZGC 的帧色容器。
四个确定性测试在握手前构造旧色旧地址，经真实 CompactRegion 产生 from/to，再调用产品 TraceHeap → handshake → GcPhaseEnum；断言读取产品 old bitmap 及原槽。未手工给 mark/discover/enqueue 输入结果。
测试的 native frame root 负责确定性进入同一 C1–C4 callback；这证明 callback 的输入/输出合同，不证明 managed 栈映射解析器或 #498 的 lazy 历史色基础设施。

## 重叠键判据与实测

`clear_entries_product_unit.cpp:2960` 的产品 CompactRegion 产生 first.from=start+size → first.to=start、second.from=start+2*size → second.to=start+size。
因此 second.to 与 first.from 数值相同，对象身份不同。

- IncomingNew(second.to) 必须保持 second.to。真实入口测试 `:3158` 经 RegisterExportRoot → CrossAccessBarrier；恢复重复 ForwardObject 时 `B09_OVERLAP_TARGET_ASSERT current_identity=0`，目标断言 `:2981` 被执行。
- OverwritePrevious(second.from) 必须转成 second.to；minor/major 两条路径与容器 rekey 都有既有用例。
- 已 current 的 old 根只翻 young relocation 色仍保留；翻所属 old 色后按旧输入处理。

本轮远端根 `kkk2:/root/sym_cangjie_runtime_596_implement_r5673746875`。
`a2-{baseline,export,green,thread-cut,export-cut,restored}/results.json` 保存每项每次 rc/目标输出/ELF/SO 哈希/核域/两端 uptime，19 项各 N=3。
四线程路径修前均 (current_marked,stale_marked,healed)=(0,1,0)，修后和恢复均 (1,0,1)；thread-cut 恢复 (0,1,0)。export 修前及 export-cut identity=0，修后/恢复=1。
NativeRootCurrent.MajorSeed 为标记读取阳性；无关 value 项在断线臂保留通过。原 finalizer 同 ELF 原64断言在两构型各 N=3：green/restored=64，old-strong-cut/finalizer-cut=0。

## FALSIFIED

原「握手在 mark 前已经建立 current」被四路径历史根实测证伪；baseline 结果和原报告保留。这是本轮修改 PushHeapRoot 输入合同的依据。
旧报告对 stack/value scanner 的基础设施说明没有覆盖 A2，不据前轮 finalizer 正反臂签发线程根结论。
