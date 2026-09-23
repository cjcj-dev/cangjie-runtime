// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Inspector/CjAllocData.h"
#include "Inspector/HeapSnapshotJsonSerializer.h"

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
