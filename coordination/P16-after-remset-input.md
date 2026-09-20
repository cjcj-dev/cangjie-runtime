待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest。

P16剩余 after-relocation/after-scan 两条 remset guarantee 的输入边界。
坐标32d6e7e84714384163c595bf209f45806f124ad6（随后ed213仅测试）。ZGC zVerify.cpp:669-718；我方zVerify.cpp:381-402。两个目标：缺位图条目必须处于young mark，且forwarding published数组包含目的字段。

真实消费者：zRelocate.cpp:1792-1798 ForwardRegion范围析构→AfterRelocation→AfterRelocationInternal；zRemembered.cpp:360 scan_forwarding之后AfterScan。前者outer生命周期待#727，之前裁定保留；本包不重做。

拟按已接受的RelocationEntryRejectsInactiveRemset同一phase-entry unit级别：GcVerifyFixture建立合法旧页/对象/forwarding，通过真实RegionManager::ForwardRegion生成结果；GDB在AfterRelocationInternal入口只改真实目的字段remembered颜色（源字段原先11且无remset，故before校验通过）。不改generation phase。这能独立验证“非young mark不能漏remset”。不是整条托管GC证明，报告明确限定。

剩余“young mark中必须published包含”及AfterScan分支需要young mark/old relocate重叠。现有ConcurrentGCBreakpoints只有major断点；直接PublishPhase伪造阶段不接受。是否允许以实际young.mark_start/remembered scan任务建立phase-unit（仍真实产品方法/消费者，明确非完整driver链），或按#727生命周期接续保留为未资格覆盖？请给本包可交付边界，避免把组件阶段用例冒充完整driver证据。不会削弱CHECK或加产品hook。
