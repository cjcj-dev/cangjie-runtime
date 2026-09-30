LANE=sym_cangjie_runtime_1337_implement_r5912596275
问题：精确转红要求“记录探测次数”，但常备裁决4禁止新增测试钩子/计数/回调。现有 forwarding at/next/insert 无探测次数观测口。请裁决允许的观测方式：可否用 gdb 对真实产品 at/next 的断点计数（不加产品计数），以 cursor 返回状态配合探测次数断言？若不允许，请提供无新增计数的验收口径。
另：不变量②要求 ZAttachedArray 只有 ZGC 接口，正文同时指出 ArrayT 为 std::atomic<uint64_t> 而非 ZForwardingEntry；将 entry 存储切为 ForwardingEntry 涉及 at/CAS 与构造，请确认本包是否应迁移该数据形态，或仅删接口与重复路由。当前 #1315 测试 alloc 重载已删除。
证据：runtime/src/Heap/z/zForwarding.hpp:62；runtime/src/Heap/z/zForwarding.inline.hpp:95,152；任务书精确转红要求、常备裁决4。
