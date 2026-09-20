本轮已有实证：接DoLeaveSaferegion→StackWatermarkSet::on_safepoint后，5个注解入口（含TypeInfo）真实collection均before!=after，返回新地址且数据拷贝正确；JNI raw-holder对照仍PASS。日志kkk2:/root/sym_cangjie_runtime_581_implement_r5748887475_head/proof，后续Type日志在_args/default/unit.log。
现在规划精确红臂：
A. saferegion消费者可切冻结36bb基线已有Mutator.inline.h:37 DoLeaveSaferegion调用（整个过渡点），预期回到旧native根不更新，注解目标红；JNI exporter保持通过。入口phase清单没有LeaveSaferegion，但仍附既有JNI pause:block独立刀满足真实相位入口。
B. 反射ABI参数重新取址新增在ArgValue::GetData；Handle producer新增在AddCJArg。恢复旧行为的最小刀是把新AddHandle换回AddInt64(raw内部地址)，或GetData不重填新handles数组。它们必然修改本轮新增行，无法通过§2.3.1“每行在冻结基线存在”的绝对判据。切旧native-root StorePlain会连测试输入根一起破坏，不能拿它代替ABI重填精确切断。
请求明确裁定B：是否允许以本轮已提交的Handle中间态为该新增接线的cut基线（不是改变DIFF/主线基线），或允许新消费者行的控制臂另附身份/真实入口/结果闭环，旧相位承重仍用JNI切刀检查？我不改entry_cut_check尺，不把无效刀报成有效，继续构造AddCJArg真实allocation-stall反例。
