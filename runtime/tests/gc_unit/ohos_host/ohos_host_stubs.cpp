// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Inspector/CjAllocData.h"
#include "Inspector/HeapSnapshotJsonSerializer.h"

// A full-nm receipt lets the runner distinguish this deliberate hybrid from a
// default Linux product. It is data rather than an API and is linked only when
// MRT_GC_UNIT_OHOS_HOST is ON.
extern "C" const char MRT_GC_UNIT_OHOS_HOST_RECEIPT[] =
    "x86_64-linux-product-with-__OHOS__";

namespace MapleRuntime {

CjAllocData* CjAllocData::GetCjAllocData()
{
    static CjAllocData data;
    return &data;
}

void CjAllocData::RecordAllocNodes(const TypeInfo*, MSize)
{
}

bool CjHeapDataForIDE::Serialize()
{
    return true;
}

void ProfilerAgentImpl(const std::string&, std::function<void(const std::string&)>)
{
}

} // namespace MapleRuntime

extern "C" unsigned long long CJ_GetUIThreadStackTop()
{
    return 0;
}

extern "C" void CJ_PushUIThreadStackTop(unsigned long long)
{
}

extern "C" void CJ_PopUIThreadStackTop()
{
}
