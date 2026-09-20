坐标基于 36bb2fc7a935361ea0cc30f7f76d130f2c35ef65。
| producer | 可执行 GC 的边界 | consumer | 必须先做 |
| MethodInfo.cpp:46/120; FieldInfo.cpp:152/264; MClass.cpp:893 分配注解包装 | ApplyCangjieMethodStub | WriteStruct(obj) 与 return obj | 回调前 Handle 登记，回调后 Handle 取址 |
| CompilerCalls.cpp:737 trace | :759–761 分配字符串 | :766–770 填充 trace | trace 和前两字符串 Handle |
| CompilerCalls.cpp:804 allRecords | :814–816 构造名字与 trace | :820–827 填充数组 | allRecords 和 name Handle |
| CompilerCalls.cpp:864 snapshot.name | :866 CreateStackTrace / :869 LeaveSaferegion | :870 return snapshot | 恢复运行态后构造并用 Handle |
| MethodInfo.cpp:430 structArgObj | 后续参数分配 / ApplyCJMethodImpl | ABI 内部地址与回调结果 | 需原生记录/派生地址迁移，已问 advisor |
ZGC: runtime/handles.hpp:65 Handle 间接槽；handles.inline.hpp:37 构造登记；reflection.cpp:796/811–817 跨分配后 mh()；gc/z/zStackWatermark.cpp:163–169 扫线程根。
