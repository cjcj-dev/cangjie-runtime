待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标：c3973505c171aa7e72095c3357ffd57f56156707。实现前接线表；尚无运行结论。

|生产端|顺序与消费者|ZGC锚|
|---|---|---|
|zPage.inline.hpp:1846 InitRegionInfo|age/owner → birth及other序号 → SetUnitRole → TLAB/shared/LARGE发布 → mark/live/holder/selector查询|zPage.cpp:90-112|
|zGeneration.cpp:96 DoYoungGarbageCollection|FlushAllocationRegions(:112)及RetireSharedPages → owner序号推进 → PublishPhase → StartYoungMarkWork → 根枚举|zGeneration.cpp:855-880|
|zGeneration.cpp:118 major分支|old快捷页退休 → old序号推进 → 相位发布 → StartOldMarkWork|zGeneration.cpp:1212-1237|
|zMark.cpp:1057/1489 本代ObjectIfActive|相位门 → current输入的MarkDomain入口 → allocating早退 → GCThread claim / AnyThread query → entry发布|zGeneration.inline.hpp:118-129; zMark.inline.hpp:48-87|
|zMark.cpp:1007/1421 worker|partial分流 → object → entry.mark竞争失败返回 → inc_live → follow|zMark.cpp:403-432|
|zMark.cpp:1241 MarkNewObject|已初始化新对象 → 本代AnyThread/DontFollow入口|zMark.inline.hpp:48-87|
|zObjectAllocator.cpp:358 pinned旧槽|删除分配侧旧槽重用和显式mark；保留资源清理|zObjectAllocator.cpp:238-249|
|zPage.inline.hpp:329/1165 FROM发布|冻结源页birth与epoch → FROM位图消费者；不得读TO新birth|zPage.inline.hpp:180-186|

消费者修改必须在替换水位资格时同批落地。未获证根/字段调用接口保留给P2/P3，不标为形态已完成。
