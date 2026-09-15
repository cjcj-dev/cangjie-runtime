LANE=sym_cangjie_runtime_606_implement_r5674249495
ROLE=implement
问题：是否批准将旧代 AssembleGarbageCandidates 从 mark-start 移至 PostTrace 选择点、删除其 ClearLiveInfo 并按 owner/relocatable 整理旧代列表，保持 ZGC young→old 顺序和原 CHECK？
R2 实测新增失败（不是 P3 放行项）：default/filler 530项529通过1既有失败；testable720项710通过10失败，其中9项新失败都在 runtime/src/Heap/Allocator/zForwardingTable.cpp:115 CHECK(region->GetOwnerGeneration()==gen)。日志 kkk2:/root/sym_cangjie_runtime_606_implement_r5674249495-green3/unit-testable.log:3508-3635。
源码因果：按 ZGC 改调用层为 young.StartYoungMark → old.StartOldMark；young 的 PrepareYoungGarbageCandidates（zRelocationSetSelector.cpp:159）先把共享 fromRegionList 中旧页停到 unmovable，再填入 young；随后旧 StartOldMark 保留原 AssembleGarbageCandidates（本候选 zGeneration.cpp:140），AssembleSmallGarbageCandidates:63-101 把 old 页又并回共享 fromRegionList。young BeginForwardingArena 如实拒绝这些 old 页。原来 old→young 穿插顺序刚好用后执行的 young prepare 修正了这个集合。
ZGC zGeneration.cpp:855-880/1212-1237 的 mark_start 根本没有 selection；select_relocation_set:205-225 才通过按代 iterator 构建集合。该旧集合整理是 C 基础设施，不应夹带改变另一代活动集合。
请裁定最小同形修法：A 将 old AssembleGarbageCandidates 从 mark_start 移到 old PostTrace 的 select/collect 之前，同时去掉那三组 Assemble* 中 ClearLiveInfo（页 mark seq 已惰性失效，post-mark 不能重置位图），按 owner/relocatable 选 old 页，禁止把 young 加入 old collection/cleanup；或 B 另有既定 P14/selector 包负责，我本包必须使用其接线。不能回退 ZGC young→old 顺序，也不能放宽 CHECK/过滤 arena 接收者掩盖错误。停下这处改动等裁决，其余 R1/红臂/OHOS 装置继续。
另托管 runner 当前只有 SATB 独立 ELF 缺测试 fixture Advance 定义的链接失败，将测试 helper 放到单独 test .cpp 并让两个测试入口链接它，保持产品不含该旁路。OHOS 真实尝试 configure rc=1，根因为 CMake probe 缺 schedule.h，将沿现有 build.py --gc-unit-ohos-host 配方补齐实际输入，不弱化探针。
