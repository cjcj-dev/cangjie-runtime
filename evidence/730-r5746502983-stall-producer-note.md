Stall producer: Heap::alloc_page flags → TakeRegion:810 allowSaferegion && !flags.non_blocking → :836 enqueue/:838 StallAllocation。
新增真实任务测试先持有 small 页，再请求最大容量 large 页（单个请求大小合法但剩余容量不足），读取返回页与 GC sequence；non_blocking 应返回 nullptr 且 sequence 未变。切 TakeRegion:810 忽略 non_blocking 后，若路径真实经过 GC，after>before 进入目标失败；不是超时/异常判红。
