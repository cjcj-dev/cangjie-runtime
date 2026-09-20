待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。
坐标67b26b0915ca599e359c651380101257c97d2f48，已接main 8cbac1ea8。

先列生产/消费顺序（实现前）：
|生产/入口|消费|P16落点|
|---|---|---|
|zPageAllocator.inline.hpp:267普通选中forwarding / :274请求选中页|zRelocate.cpp:1245 ForwardClaimedPage claim与PageWorkScope|在唯一已claim页入口做BeforeRelocation|
|ForwardClaimedPage→CompactRegion / ForwardRegion→RelocateClaimedPage|四处VerifyRelocatedPage；ForwardRegion独有AfterRelocation RAII|合到两分支汇合后AfterRelocation→ZVerifyForwarding→owner.verify，ZGC zRelocate.cpp:993-1008；直接in-place同样经过|
|验证完成|generation统计→old remset发布→release_page→detach_page→Heap::free_page→mark_done/queue.complete|保留#727源页退休顺序（ZGC zRelocate.cpp:1009-1047），不再从已退休页查询forwarding|

旧VerifyRelocatedPage包装与四调用删除，ForwardRegion的before/RAII删除。原fixture切换为实际ForwardFromRegions→ExecuteForwardTask普通分支（已在冻结325d6ad7存在），在此入口切断ForwardClaimedPage调用，避免仅测旧叶函数。in-place目的页remset检查已按主线clone/reset更新为toPage（ZGC:877-886）。
