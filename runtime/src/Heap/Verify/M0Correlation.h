// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_M0_CORRELATION_H
#define MRT_M0_CORRELATION_H

#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class BaseObject;

namespace M0Correlation {

using AllocationToken = uint64_t;

struct ObjectStamp {
    bool valid { false };
    uintptr_t address { 0 };
    uintptr_t regionStart { 0 };
    uint64_t regionLife { 0 };
    uintptr_t offset { 0 };

    bool operator==(const ObjectStamp& other) const
    {
        return valid == other.valid && address == other.address && regionStart == other.regionStart &&
            regionLife == other.regionLife && offset == other.offset;
    }
};

struct EndpointEvidence {
    bool present { false };
    AllocationToken token { 0 };
    ObjectStamp stamp;
};

// Observation `claimed_*` fields are the consumer's claim.  They are not
// runtime endorsement; reconciliation decides whether the two endpoints agree.

enum class BindingInvalidation : uint8_t {
    REGION_REUSE = 0,
    PINNED_SLOT_REUSE = 1,
};

#if defined(MRT_M0_CORRELATION_EXPERIMENT)

bool Enabled();
ObjectStamp CaptureStamp(const BaseObject* object);
ObjectStamp CaptureStamp(MAddress address);

void TagNextAllocation(uint64_t externalKey);
void ConsumeMoveableAllocation(BaseObject* object);
void RejectPendingTag(const char* reason);
void Observe(BaseObject* consumedObject, uint64_t externalKey, uint64_t consumerToken,
             uint64_t payload0, uint64_t payload1);
void Release(uint64_t externalKey);
void SelectKeepLive(uint64_t externalKey);
bool IsSelected(uint64_t externalKey);

void PropagateForwarding(MAddress from, MAddress to, MAddress receipt, bool installed);
void InvalidateRegionBindings(MAddress regionStart, uint64_t oldLife);
void InvalidateStampBinding(MAddress address, BindingInvalidation reason);

uint64_t NextCausalSeq();
void RecordM0(uint64_t causalSeq, uint64_t m0Seq, const char* exitName, const char* classification,
              BaseObject* target, MAddress activeTo, MAddress retiredTo, uint8_t phase);

#if defined(MRT_GC_UNIT_TEST_ACCESS)
struct TestSnapshot {
    uint64_t binds;
    uint64_t tagRejected;
    uint64_t forwards;
    uint64_t m0Seen;
    uint64_t m0Written;
    uint64_t observations;
    uint64_t releases;
    uint64_t interventionRegister;
    uint64_t interventionUnregister;
    uint64_t contractErrors;
    uint64_t writeErrors;
};

void ResetForTest();
AllocationToken BindStampForTest(uint64_t externalKey, const ObjectStamp& stamp);
AllocationToken LookupStampForTest(const ObjectStamp& stamp);
AllocationToken ExternalTokenForTest(uint64_t externalKey);
bool ValidateEndpointForTest(bool present, const ObjectStamp& stamp);
const char* ClassifyEvidenceForTest(bool targetPresent, const ObjectStamp& target,
                                    bool consumerPresent, const ObjectStamp& consumer,
                                    bool activeToPresent, const ObjectStamp& activeTo,
                                    bool retiredToPresent, const ObjectStamp& retiredTo);
void DropNextM0WriteForTest();
bool FooterValidForTest();
TestSnapshot SnapshotForTest();
#endif

#else

inline bool Enabled() { return false; }
inline ObjectStamp CaptureStamp(const BaseObject*) { return {}; }
inline ObjectStamp CaptureStamp(MAddress) { return {}; }
inline void TagNextAllocation(uint64_t) {}
inline void ConsumeMoveableAllocation(BaseObject*) {}
inline void RejectPendingTag(const char*) {}
inline void Observe(BaseObject*, uint64_t, uint64_t, uint64_t, uint64_t) {}
inline void Release(uint64_t) {}
inline void SelectKeepLive(uint64_t) {}
inline bool IsSelected(uint64_t) { return false; }
inline void PropagateForwarding(MAddress, MAddress, MAddress, bool) {}
inline void InvalidateRegionBindings(MAddress, uint64_t) {}
inline void InvalidateStampBinding(MAddress, BindingInvalidation) {}
inline uint64_t NextCausalSeq() { return 0; }
inline void RecordM0(uint64_t, uint64_t, const char*, const char*, BaseObject*, MAddress, MAddress, uint8_t) {}

#endif

} // namespace M0Correlation
} // namespace MapleRuntime

#endif // MRT_M0_CORRELATION_H
