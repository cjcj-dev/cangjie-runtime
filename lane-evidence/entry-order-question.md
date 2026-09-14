LANE=sym_cangjie_runtime_581_implement_r5668277053
返工B1要求真实CompactRegion晋升、位移from/to后经过MCC_AcquireRawData/PinArray。源码发现前置读取问题，需裁定范围/有效测试时序：
- runtime/src/Heap/z/zRelocate.cpp:2094-2097，CompactRegion完成复制及发表后，对尾部旧from区执行HeapFiller::ZeroAndFill。
- runtime/src/Heap/Allocator/HeapFiller.cpp:86-92 先memset清除，再依构型Overlay。
- runtime/src/CompilerCalls.cpp:960 先plain->GetContentSize()，:963 先IsPrimitiveArray()，:969 才PinArray。
- runtime/src/ObjectModel/MArray.inline.h:47-52 GetContentSize无条件经GetElementSize读取component TypeInfo；:38-44读取component。
因此真实位移后的from不能作为有效数组头直接进入Acquire，可能在Pin之前失败；手动恢复from头会再次破坏真实输入闭环，不能这样做。
请裁定：(A) 本条允许把Acquire中数组头读取移到产品解析后（release同样有旧array的前置类型读取），并补空数组pin计数配对；或(B) 此入口前置读取另立问题，本条用真实晋升后的Pin输入证明消费，Acquire独立用真实可读取数组输入证明虚调用，明确不能声称同一调用闭环；或指定可复现的合法生产时序。尚未改码，继续读证。本条不修改#579页代真值源，不放宽CHECK。
