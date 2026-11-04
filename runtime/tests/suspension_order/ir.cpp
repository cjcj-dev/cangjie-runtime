// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Mutator/Mutator.inline.h"

// Consume the actual product header, including the getter without a live caller.
extern "C" uint32_t R06GetFlag(const MapleRuntime::Mutator* mutator)
{
    return mutator->GetSuspensionFlag();
}
extern "C" bool R06HasSync(const MapleRuntime::Mutator* mutator)
{
    return mutator->HasSuspensionRequest(MapleRuntime::Mutator::SUSPENSION_FOR_SYNC);
}
extern "C" bool R06HasAny(const MapleRuntime::Mutator* mutator)
{
    return mutator->HasAnySuspensionRequest();
}
