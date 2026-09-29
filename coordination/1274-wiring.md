待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
# #1274 修改前 producer → consumer 表
坐标基于 279a021724838c0d90580bc5251040972be42f4f。

| producer | consumer / 顺序 | 必须在何处之前分路 |
|---|---|---|
| MFuncDesc::GetStackMap / DataRefOffset32::GetDataRef (`runtime/src/Common/Dataref.h:27-32`) | CompressedStackMapHead::GetStackMapHead (`runtime/src/StackMap/CompressedStackMap.h:131-145`) | FramePrologue 构造/Read 之前 |
| GetStackMapHead | StackMapBuilder::Build 通用版 :280-287、HeapReferenceMap 特化 :317-323、MethodMap 特化 :332-341、GetInvalidReason :296-300 | 无 map 返回 invalid / 空 closure，不读表 |
| Build<HeapReferenceMap> | StackFrameStream::CheckRegisterRoots (`runtime/src/UnwindStack/StackInfo.cpp:283-286`) | 保持 roots.IsValid() 判据 |
| Build<HeapReferenceMap> | ZStackWatermark::process_frame (`runtime/src/Heap/z/zStackWatermark.cpp:104-107`) | 保持 !pointers.IsValid() 返回，外层遍历继续 |

规格：`/root/cj_build/reference/jdk/src/hotspot/share/runtime/frame.cpp:995-1011` 在 oop_map()!=nullptr 后才扫描；`runtime/stackWatermark.inline.hpp:42-59` 允许无 barrier 帧。
任务书明确排除 frame size/ABI/has_barrier 改动。头保存可空元数据指针；只在非空时构造局部 FramePrologue，保持 MethodMap 路径不分配堆内存，不制造 frameSize=0。
