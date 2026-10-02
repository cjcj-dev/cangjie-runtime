待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`
LANE=sym_cangjie_runtime_1478_implement_r5963025986
固定起点 d164ba01a6cc7730746f3d607b50b5006c80f5ed；本轮 N0，产品源码不可改。
Windows内部registered hole的适用边界请裁：ElfUnloadQuiescence.cpp:556-568 实际生产者 VirtualQuery 按同 AllocationBase 的 MEM_COMMIT RegionSize 全量登记，无 PE VirtualSize/RawSize 字节收缩。ImageDirectory::Rebuild 归一化后 MFuncdesc.inline.h:73-77 查每字节；FrameInfo.cpp:77真实调用该变体。四字节prefix在页粒度MEM_COMMIT边界可跨连续两region，但首尾同owner、中间两字节不属于owner的布局不能由此生产者产生：连续页内region无两字节孔；页级孔宽度超过prefix。不能改PE section header制造精确两字节注册孔；不能用未映射读造成的失败证明资格拒绝。
Linux ElfUnloadQuiescence.cpp:605-620 则登记实际PT_LOAD p_memsz，不做页扩张；既有 a2_contiguous.ld 和 a2_prefix_hole.py 可生成精确两字节孔。ARM64共享该生产者和FrameInfo.cpp:77消费者，但合法真实装载尚未运行，N=0。
问题：Windows保留普通prefix及连续VirtualQuery region并集的真实消费者控制，将两字节内部registered hole标为格式/生产者不适用，附上述源码反证，是否符合本次候选范围？或有平台合法输入需指定？不宣称产品缺能力，不新建框架，不新增执行额度。后续旧首尾刀Windows无法仅使内部gap红，不能冒该因果臂已闭合。
本轮等待裁定期间仅准备既有工程Linux输入迁移和普通prefix资格输出，不编译/执行/push/GHA/merge主线。
