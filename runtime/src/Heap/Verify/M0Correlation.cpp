// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Verify/M0Correlation.h"

#if defined(MRT_M0_CORRELATION_EXPERIMENT)

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Cangjie.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace M0Correlation {
namespace {

struct StampHash {
    size_t operator()(const ObjectStamp& stamp) const
    {
        size_t h = static_cast<size_t>(stamp.address);
        h ^= static_cast<size_t>(stamp.regionStart) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= static_cast<size_t>(stamp.regionLife) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= static_cast<size_t>(stamp.offset) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

struct Transition {
    uint64_t ordinal { 0 };
    ObjectStamp from;
    ObjectStamp to;
    MAddress receipt { 0 };
};

struct Lineage {
    uint64_t externalKey { 0 };
    ObjectStamp initial;
    ObjectStamp current;
    std::vector<Transition> transitions;
    bool released { false };
    U64 exportRootId { std::numeric_limits<U64>::max() };
};

std::mutex g_registryLock;
std::unordered_map<ObjectStamp, AllocationToken, StampHash> g_current;
std::unordered_map<AllocationToken, Lineage> g_lineage;
std::unordered_map<uint64_t, AllocationToken> g_external;
std::vector<std::pair<AllocationToken, Transition>> g_forwardFrom;
uint64_t g_nextAllocationToken = 1;
uint64_t g_nextTransition = 1;
uint64_t g_selectedExternalKey = 0;
bool g_selectorSet = false;
thread_local uint64_t g_pendingExternalKey = 0;

std::mutex g_sinkLock;
uint64_t g_ledgerSeq = 0;
std::atomic<uint64_t> g_causalSeq { 0 };
std::atomic<uint64_t> g_binds { 0 };
std::atomic<uint64_t> g_tagRejected { 0 };
std::atomic<uint64_t> g_forwards { 0 };
std::atomic<uint64_t> g_m0Seen { 0 };
std::atomic<uint64_t> g_m0Written { 0 };
std::atomic<uint64_t> g_observations { 0 };
std::atomic<uint64_t> g_releases { 0 };
std::atomic<uint64_t> g_interventionRegister { 0 };
std::atomic<uint64_t> g_interventionUnregister { 0 };
std::atomic<uint64_t> g_contractErrors { 0 };
std::atomic<uint64_t> g_writeErrors { 0 };
std::atomic<bool> g_footerRegistered { false };
#if defined(MRT_GC_UNIT_TEST_ACCESS)
std::atomic<bool> g_dropNextM0Write { false };
#endif

const char* BoolDigit(bool value)
{
    return value ? "1" : "0";
}

bool StampArithmeticValid(const ObjectStamp& stamp)
{
    if (!stamp.valid || stamp.address == 0 || stamp.regionStart == 0 || stamp.regionLife == 0 ||
        stamp.address < stamp.regionStart) {
        return false;
    }
    if (stamp.offset > std::numeric_limits<uintptr_t>::max() - stamp.regionStart) {
        return false;
    }
    return stamp.address == stamp.regionStart + stamp.offset;
}

bool EndpointValid(bool present, const ObjectStamp& stamp)
{
    return !present || StampArithmeticValid(stamp);
}

uint64_t Emit(const char* record, const char* format, ...)
{
    char fields[3584];
    va_list args;
    va_start(args, format);
    const int fieldLength = std::vsnprintf(fields, sizeof(fields), format, args);
    va_end(args);
    if (fieldLength < 0 || static_cast<size_t>(fieldLength) >= sizeof(fields)) {
        g_writeErrors.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_sinkLock);
    if (g_ledgerSeq == std::numeric_limits<uint64_t>::max()) {
        g_writeErrors.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const uint64_t seq = ++g_ledgerSeq;
    const int rc = std::fprintf(stderr, "[M0CORR] schema=1 rec=%s ledger_seq=%llu %s\n", record,
                                static_cast<unsigned long long>(seq), fields);
    if (rc < 0 || std::fflush(stderr) != 0) {
        g_writeErrors.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return seq;
}

bool FooterValid()
{
    const uint64_t m0Seen = g_m0Seen.load(std::memory_order_relaxed);
    const uint64_t m0Written = g_m0Written.load(std::memory_order_relaxed);
    const uint64_t errors = g_contractErrors.load(std::memory_order_relaxed);
    const uint64_t writes = g_writeErrors.load(std::memory_order_relaxed);
    const uint64_t registered = g_interventionRegister.load(std::memory_order_relaxed);
    const uint64_t unregistered = g_interventionUnregister.load(std::memory_order_relaxed);
    uint64_t selected = 0;
    {
        std::lock_guard<std::mutex> lock(g_registryLock);
        selected = g_selectedExternalKey;
    }
    const bool interventionValid = selected == 0 ? (registered == 0 && unregistered == 0)
                                                  : (registered == 1 && unregistered == 1);
    return errors == 0 && writes == 0 && m0Seen == m0Written && interventionValid;
}

void DumpFooter()
{
    const uint64_t m0Seen = g_m0Seen.load(std::memory_order_relaxed);
    const uint64_t m0Written = g_m0Written.load(std::memory_order_relaxed);
    const uint64_t errors = g_contractErrors.load(std::memory_order_relaxed);
    const uint64_t writes = g_writeErrors.load(std::memory_order_relaxed);
    const uint64_t registered = g_interventionRegister.load(std::memory_order_relaxed);
    const uint64_t unregistered = g_interventionUnregister.load(std::memory_order_relaxed);
    (void)Emit("footer",
               "valid=%s binds=%llu tag_rejected=%llu forwards=%llu m0_seen=%llu m0_written=%llu "
               "observations=%llu releases=%llu intervention_register=%llu intervention_unregister=%llu "
               "contract_errors=%llu write_errors=%llu",
               BoolDigit(FooterValid()), static_cast<unsigned long long>(g_binds.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(g_tagRejected.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(g_forwards.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(m0Seen), static_cast<unsigned long long>(m0Written),
               static_cast<unsigned long long>(g_observations.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(g_releases.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(registered), static_cast<unsigned long long>(unregistered),
               static_cast<unsigned long long>(errors), static_cast<unsigned long long>(writes));
}

void EnsureFooter()
{
    bool expected = false;
    if (g_footerRegistered.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        (void)std::atexit(DumpFooter);
    }
}

void ContractError()
{
    g_contractErrors.fetch_add(1, std::memory_order_relaxed);
    EnsureFooter();
}

AllocationToken MintTokenLocked()
{
    if (g_nextAllocationToken == std::numeric_limits<uint64_t>::max()) {
        std::fprintf(stderr, "[M0CORR] fatal=allocation_token_overflow\n");
        std::fflush(stderr);
        std::abort();
    }
    return g_nextAllocationToken++;
}

const char* InvalidationName(BindingInvalidation reason)
{
    return reason == BindingInvalidation::REGION_REUSE ? "region_reuse" : "pinned_slot_reuse";
}

void EmitRetire(AllocationToken token, const ObjectStamp& stamp, const char* reason)
{
    (void)Emit("retire",
               "allocation_token=%llu stamp.valid=%s stamp.address=%#llx stamp.region_start=%#llx "
               "stamp.region_life=%llu stamp.offset=%#llx reason=%s",
               static_cast<unsigned long long>(token), BoolDigit(stamp.valid),
               static_cast<unsigned long long>(stamp.address), static_cast<unsigned long long>(stamp.regionStart),
               static_cast<unsigned long long>(stamp.regionLife), static_cast<unsigned long long>(stamp.offset),
               reason);
}

AllocationToken BindStampLocked(uint64_t externalKey, const ObjectStamp& stamp, BaseObject* object)
{
    if (externalKey == 0 || !StampArithmeticValid(stamp) || g_external.find(externalKey) != g_external.end()) {
        ContractError();
        return 0;
    }
    std::unordered_map<ObjectStamp, AllocationToken, StampHash>::iterator occupied = g_current.find(stamp);
    if (occupied != g_current.end()) {
        const AllocationToken old = occupied->second;
        EmitRetire(old, stamp, "new_allocation_overwrite");
        std::unordered_map<AllocationToken, Lineage>::iterator oldLineage = g_lineage.find(old);
        if (oldLineage != g_lineage.end()) {
            oldLineage->second.current = {};
        }
        g_current.erase(occupied);
    }

    const AllocationToken token = MintTokenLocked();
    Lineage lineage;
    lineage.externalKey = externalKey;
    lineage.initial = stamp;
    lineage.current = stamp;

    if (g_selectedExternalKey == externalKey) {
        if (object == nullptr) {
            ContractError();
            return 0;
        }
        const U64 id = Heap::GetHeap().RegisterExportRoot(object);
        if (id == std::numeric_limits<U64>::max()) {
            ContractError();
            return 0;
        }
        lineage.exportRootId = id;
        g_interventionRegister.fetch_add(1, std::memory_order_relaxed);
        (void)Emit("intervention", "kind=keep-live external_key=%llu allocation_token=%llu action=register",
                   static_cast<unsigned long long>(externalKey), static_cast<unsigned long long>(token));
    }

    g_lineage.emplace(token, lineage);
    g_external.emplace(externalKey, token);
    g_current[stamp] = token;
    g_binds.fetch_add(1, std::memory_order_relaxed);
    (void)Emit("bind",
               "external_key=%llu allocation_token=%llu stamp.valid=1 stamp.address=%#llx "
               "stamp.region_start=%#llx stamp.region_life=%llu stamp.offset=%#llx",
               static_cast<unsigned long long>(externalKey), static_cast<unsigned long long>(token),
               static_cast<unsigned long long>(stamp.address), static_cast<unsigned long long>(stamp.regionStart),
               static_cast<unsigned long long>(stamp.regionLife), static_cast<unsigned long long>(stamp.offset));
    return token;
}

AllocationToken ResolveStampLocked(const ObjectStamp& stamp, MAddress relatedTo, bool requireRelation)
{
    if (!StampArithmeticValid(stamp)) {
        return 0;
    }
    std::unordered_set<AllocationToken> candidates;
    // A forwarding receipt is the discriminator when an old from stamp has
    // since become a new current occupant.  A bare current lookup is not
    // evidence for that relation and must not be allowed to win the join.
    if (!requireRelation) {
        std::unordered_map<ObjectStamp, AllocationToken, StampHash>::const_iterator current =
            g_current.find(stamp);
        if (current != g_current.end()) {
            candidates.insert(current->second);
        }
    }
    for (size_t i = 0; i < g_forwardFrom.size(); ++i) {
        const Transition& transition = g_forwardFrom[i].second;
        if (transition.from == stamp && (!requireRelation || relatedTo == transition.to.address)) {
            candidates.insert(g_forwardFrom[i].first);
        }
    }
    return candidates.size() == 1 ? *candidates.begin() : 0;
}

} // namespace

bool Enabled()
{
    static const bool enabled = DiagGate::TokenOn("m0corr");
    return enabled;
}

ObjectStamp CaptureStamp(const BaseObject* object)
{
    return CaptureStamp(reinterpret_cast<MAddress>(object));
}

ObjectStamp CaptureStamp(MAddress address)
{
    ObjectStamp stamp;
    stamp.address = static_cast<uintptr_t>(address);
    if (address == 0 || !Heap::IsHeapAddress(address)) {
        return stamp;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(address);
    if (region == nullptr) {
        return stamp;
    }
    stamp.regionStart = static_cast<uintptr_t>(region->GetRegionStart());
    stamp.regionLife = region->GetRegionLifeId();
    if (stamp.regionLife == 0 || stamp.address < stamp.regionStart) {
        return stamp;
    }
    stamp.offset = stamp.address - stamp.regionStart;
    if (stamp.offset > std::numeric_limits<uintptr_t>::max() - stamp.regionStart ||
        stamp.address != stamp.regionStart + stamp.offset) {
        return stamp;
    }
    stamp.valid = true;
    return stamp;
}

void TagNextAllocation(uint64_t externalKey)
{
    if (!Enabled()) {
        return;
    }
    EnsureFooter();
    if (externalKey == 0 || g_pendingExternalKey != 0) {
        ContractError();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_registryLock);
        if (g_external.find(externalKey) != g_external.end()) {
            ContractError();
            return;
        }
    }
    g_pendingExternalKey = externalKey;
}

void ConsumeMoveableAllocation(BaseObject* object)
{
    if (!Enabled() || g_pendingExternalKey == 0) {
        return;
    }
    EnsureFooter();
    const uint64_t externalKey = g_pendingExternalKey;
    g_pendingExternalKey = 0;
    if (object == nullptr) {
        g_tagRejected.fetch_add(1, std::memory_order_relaxed);
        ContractError();
        (void)Emit("tag_rejected", "external_key=%llu reason=allocation_failed",
                   static_cast<unsigned long long>(externalKey));
        return;
    }
    const ObjectStamp stamp = CaptureStamp(object);
    std::lock_guard<std::mutex> lock(g_registryLock);
    (void)BindStampLocked(externalKey, stamp, object);
}

void RejectPendingTag(const char* reason)
{
    if (!Enabled() || g_pendingExternalKey == 0) {
        return;
    }
    EnsureFooter();
    const uint64_t externalKey = g_pendingExternalKey;
    g_pendingExternalKey = 0;
    g_tagRejected.fetch_add(1, std::memory_order_relaxed);
    ContractError();
    (void)Emit("tag_rejected", "external_key=%llu reason=%s", static_cast<unsigned long long>(externalKey),
               reason == nullptr ? "unsupported" : reason);
}

void Release(uint64_t externalKey)
{
    if (!Enabled()) {
        return;
    }
    EnsureFooter();
    if (externalKey == 0) {
        ContractError();
        return;
    }
    std::lock_guard<std::mutex> lock(g_registryLock);
    std::unordered_map<uint64_t, AllocationToken>::iterator external = g_external.find(externalKey);
    if (external == g_external.end()) {
        ContractError();
        return;
    }
    const AllocationToken token = external->second;
    std::unordered_map<AllocationToken, Lineage>::iterator lineage = g_lineage.find(token);
    if (lineage == g_lineage.end() || lineage->second.released) {
        ContractError();
        return;
    }
    if (StampArithmeticValid(lineage->second.current)) {
        std::unordered_map<ObjectStamp, AllocationToken, StampHash>::iterator current =
            g_current.find(lineage->second.current);
        if (current != g_current.end() && current->second == token) {
            g_current.erase(current);
        }
    }
    if (lineage->second.exportRootId != std::numeric_limits<U64>::max()) {
        Heap::GetHeap().RemoveExportObject(lineage->second.exportRootId);
        lineage->second.exportRootId = std::numeric_limits<U64>::max();
        g_interventionUnregister.fetch_add(1, std::memory_order_relaxed);
        (void)Emit("intervention", "kind=keep-live external_key=%llu allocation_token=%llu action=unregister",
                   static_cast<unsigned long long>(externalKey), static_cast<unsigned long long>(token));
    }
    lineage->second.released = true;
    lineage->second.current = {};
    g_external.erase(external);
    for (std::vector<std::pair<AllocationToken, Transition>>::iterator it = g_forwardFrom.begin();
         it != g_forwardFrom.end();) {
        if (it->first == token) {
            it = g_forwardFrom.erase(it);
        } else {
            ++it;
        }
    }
    g_releases.fetch_add(1, std::memory_order_relaxed);
    (void)Emit("release", "external_key=%llu allocation_token=%llu",
               static_cast<unsigned long long>(externalKey), static_cast<unsigned long long>(token));
}

void SelectKeepLive(uint64_t externalKey)
{
    if (!Enabled()) {
        return;
    }
    EnsureFooter();
    std::lock_guard<std::mutex> lock(g_registryLock);
    if (g_selectorSet || (externalKey != 0 && g_external.find(externalKey) != g_external.end())) {
        ContractError();
        return;
    }
    g_selectorSet = true;
    g_selectedExternalKey = externalKey;
}

bool IsSelected(uint64_t externalKey)
{
    if (!Enabled() || externalKey == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_registryLock);
    return g_selectorSet && g_selectedExternalKey == externalKey;
}

void Observe(BaseObject* consumedObject, uint64_t externalKey, uint64_t consumerToken,
             uint64_t payload0, uint64_t payload1)
{
    const uint64_t causalSeq = NextCausalSeq();
    if (!Enabled()) {
        return;
    }
    EnsureFooter();
    EndpointEvidence claimedEvidence;
    EndpointEvidence consumerEvidence { true, 0, CaptureStamp(consumedObject) };
    bool valid = causalSeq != 0 && externalKey != 0 &&
        EndpointValid(consumerEvidence.present, consumerEvidence.stamp);
    const char* resultClass = "MATCH";
    U64 interventionToRemove = std::numeric_limits<U64>::max();

    {
        std::lock_guard<std::mutex> lock(g_registryLock);
        std::unordered_map<uint64_t, AllocationToken>::const_iterator external = g_external.find(externalKey);
        if (external == g_external.end()) {
            valid = false;
        } else {
            claimedEvidence.token = external->second;
            std::unordered_map<AllocationToken, Lineage>::iterator lineage = g_lineage.find(external->second);
            if (lineage == g_lineage.end() || lineage->second.released ||
                !StampArithmeticValid(lineage->second.current)) {
                valid = false;
            } else {
                claimedEvidence.present = true;
                claimedEvidence.stamp = CaptureStamp(lineage->second.current.address);
                if (!EndpointValid(true, claimedEvidence.stamp) ||
                    !(claimedEvidence.stamp == lineage->second.current)) {
                    valid = false;
                }
            }
        }

        if (valid) {
            consumerEvidence.token = ResolveStampLocked(consumerEvidence.stamp, 0, false);
            if (consumerEvidence.token == 0) {
                valid = false;
            } else if (consumerEvidence.token != claimedEvidence.token) {
                resultClass = "IDENTITY_DIVERGENCE";
                std::unordered_map<AllocationToken, Lineage>::iterator claimedLineage =
                    g_lineage.find(claimedEvidence.token);
                if (claimedLineage != g_lineage.end() &&
                    claimedLineage->second.exportRootId != std::numeric_limits<U64>::max()) {
                    interventionToRemove = claimedLineage->second.exportRootId;
                    claimedLineage->second.exportRootId = std::numeric_limits<U64>::max();
                    g_interventionUnregister.fetch_add(1, std::memory_order_relaxed);
                    (void)Emit("intervention",
                               "kind=keep-live external_key=%llu allocation_token=%llu action=unregister",
                               static_cast<unsigned long long>(externalKey),
                               static_cast<unsigned long long>(claimedEvidence.token));
                }
            }
        }
    }

    if (interventionToRemove != std::numeric_limits<U64>::max()) {
        Heap::GetHeap().RemoveExportObject(interventionToRemove);
    }
    if (!valid) {
        resultClass = "INVALID_EVIDENCE";
        ContractError();
    }
    g_observations.fetch_add(1, std::memory_order_relaxed);
    (void)Emit(
        "observation",
        "causal_seq=%llu class=%s external_key=%llu claimed_allocation_token=%llu "
        "consumer_allocation_token=%llu consumer_token=%llu payload0=%llu payload1=%llu "
        "claimed.present=%s claimed.valid=%s claimed.address=%#llx claimed.region_start=%#llx "
        "claimed.region_life=%llu claimed.offset=%#llx consumer.present=1 consumer.valid=%s "
        "consumer.address=%#llx consumer.region_start=%#llx consumer.region_life=%llu consumer.offset=%#llx",
        static_cast<unsigned long long>(causalSeq), resultClass, static_cast<unsigned long long>(externalKey),
        static_cast<unsigned long long>(claimedEvidence.token),
        static_cast<unsigned long long>(consumerEvidence.token), static_cast<unsigned long long>(consumerToken),
        static_cast<unsigned long long>(payload0), static_cast<unsigned long long>(payload1),
        BoolDigit(claimedEvidence.present), BoolDigit(claimedEvidence.stamp.valid),
        static_cast<unsigned long long>(claimedEvidence.stamp.address),
        static_cast<unsigned long long>(claimedEvidence.stamp.regionStart),
        static_cast<unsigned long long>(claimedEvidence.stamp.regionLife),
        static_cast<unsigned long long>(claimedEvidence.stamp.offset), BoolDigit(consumerEvidence.stamp.valid),
        static_cast<unsigned long long>(consumerEvidence.stamp.address),
        static_cast<unsigned long long>(consumerEvidence.stamp.regionStart),
        static_cast<unsigned long long>(consumerEvidence.stamp.regionLife),
        static_cast<unsigned long long>(consumerEvidence.stamp.offset));
}

void PropagateForwarding(MAddress from, MAddress to, MAddress receipt, bool installed)
{
    if (!Enabled()) {
        return;
    }
    EnsureFooter();
    const ObjectStamp fromStamp = CaptureStamp(from);
    const ObjectStamp toStamp = CaptureStamp(to);
    if (!StampArithmeticValid(fromStamp) || !StampArithmeticValid(toStamp) || receipt == 0 || receipt != to) {
        ContractError();
        return;
    }
    std::lock_guard<std::mutex> lock(g_registryLock);
    for (size_t i = 0; i < g_forwardFrom.size(); ++i) {
        const Transition& previous = g_forwardFrom[i].second;
        if (previous.from == fromStamp && previous.to == toStamp && previous.receipt == receipt) {
            const AllocationToken previousToken = g_forwardFrom[i].first;
            std::unordered_map<AllocationToken, Lineage>::const_iterator previousLineage =
                g_lineage.find(previousToken);
            if (previousLineage == g_lineage.end() || previousLineage->second.released) {
                ContractError();
            }
            return;
        }
    }
    std::unordered_map<ObjectStamp, AllocationToken, StampHash>::iterator source = g_current.find(fromStamp);
    if (source == g_current.end()) {
        ContractError();
        return;
    }
    const AllocationToken token = source->second;
    std::unordered_map<AllocationToken, Lineage>::iterator lineageIt = g_lineage.find(token);
    if (lineageIt == g_lineage.end() || lineageIt->second.released) {
        ContractError();
        return;
    }

    std::unordered_map<ObjectStamp, AllocationToken, StampHash>::iterator destination = g_current.find(toStamp);
    if (destination != g_current.end() && destination->second != token) {
        const AllocationToken overwritten = destination->second;
        EmitRetire(overwritten, toStamp, "forward_destination_overwrite");
        std::unordered_map<AllocationToken, Lineage>::iterator old = g_lineage.find(overwritten);
        if (old != g_lineage.end()) {
            old->second.current = {};
        }
        g_current.erase(destination);
    }

    Transition transition;
    transition.ordinal = g_nextTransition++;
    transition.from = fromStamp;
    transition.to = toStamp;
    transition.receipt = receipt;
    lineageIt->second.transitions.push_back(transition);
    lineageIt->second.current = toStamp;
    g_forwardFrom.push_back(std::make_pair(token, transition));
    if (!(fromStamp == toStamp)) {
        g_current.erase(fromStamp);
    }
    g_current[toStamp] = token;
    g_forwards.fetch_add(1, std::memory_order_relaxed);
    (void)Emit("forward",
               "allocation_token=%llu from.valid=1 from.address=%#llx from.region_start=%#llx "
               "from.region_life=%llu from.offset=%#llx to.valid=1 to.address=%#llx to.region_start=%#llx "
               "to.region_life=%llu to.offset=%#llx receipt=%#llx installed=%s",
               static_cast<unsigned long long>(token), static_cast<unsigned long long>(fromStamp.address),
               static_cast<unsigned long long>(fromStamp.regionStart),
               static_cast<unsigned long long>(fromStamp.regionLife),
               static_cast<unsigned long long>(fromStamp.offset), static_cast<unsigned long long>(toStamp.address),
               static_cast<unsigned long long>(toStamp.regionStart),
               static_cast<unsigned long long>(toStamp.regionLife), static_cast<unsigned long long>(toStamp.offset),
               static_cast<unsigned long long>(receipt), BoolDigit(installed));
}

void InvalidateRegionBindings(MAddress regionStart, uint64_t oldLife)
{
    if (!Enabled() || regionStart == 0 || oldLife == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_registryLock);
    for (std::unordered_map<ObjectStamp, AllocationToken, StampHash>::iterator it = g_current.begin();
         it != g_current.end();) {
        if (it->first.regionStart == regionStart && it->first.regionLife == oldLife) {
            EmitRetire(it->second, it->first, InvalidationName(BindingInvalidation::REGION_REUSE));
            std::unordered_map<AllocationToken, Lineage>::iterator lineage = g_lineage.find(it->second);
            if (lineage != g_lineage.end()) {
                lineage->second.current = {};
            }
            it = g_current.erase(it);
        } else {
            ++it;
        }
    }
}

void InvalidateStampBinding(MAddress address, BindingInvalidation reason)
{
    if (!Enabled() || address == 0) {
        return;
    }
    const ObjectStamp stamp = CaptureStamp(address);
    if (!StampArithmeticValid(stamp)) {
        ContractError();
        return;
    }
    std::lock_guard<std::mutex> lock(g_registryLock);
    std::unordered_map<ObjectStamp, AllocationToken, StampHash>::iterator it = g_current.find(stamp);
    if (it != g_current.end()) {
        EmitRetire(it->second, stamp, InvalidationName(reason));
        std::unordered_map<AllocationToken, Lineage>::iterator lineage = g_lineage.find(it->second);
        if (lineage != g_lineage.end()) {
            lineage->second.current = {};
        }
        g_current.erase(it);
    }
    // A reused pinned slot is outside the moveable candidate domain.  Even a
    // historical from/to version at the identical stamp must not make a
    // receipt-free consumer lookup resolve to the old occupant afterwards.
    if (reason == BindingInvalidation::PINNED_SLOT_REUSE) {
        for (std::vector<std::pair<AllocationToken, Transition>>::iterator transition = g_forwardFrom.begin();
             transition != g_forwardFrom.end();) {
            if (transition->second.from == stamp || transition->second.to == stamp) {
                transition = g_forwardFrom.erase(transition);
            } else {
                ++transition;
            }
        }
    }
}

uint64_t NextCausalSeq()
{
    if (!Enabled()) {
        return 0;
    }
    EnsureFooter();
    const uint64_t previous = g_causalSeq.fetch_add(1, std::memory_order_relaxed);
    if (previous == std::numeric_limits<uint64_t>::max()) {
        std::fprintf(stderr, "[M0CORR] fatal=causal_seq_overflow\n");
        std::fflush(stderr);
        std::abort();
    }
    return previous + 1;
}

void RecordM0(uint64_t causalSeq, uint64_t m0Seq, const char* exitName, const char* classification,
              BaseObject* target, MAddress activeTo, MAddress retiredTo, uint8_t phase)
{
    if (!Enabled()) {
        return;
    }
    EnsureFooter();
    g_m0Seen.fetch_add(1, std::memory_order_relaxed);
    EndpointEvidence targetEvidence { true, 0, CaptureStamp(target) };
    EndpointEvidence activeEvidence { activeTo != 0, 0, CaptureStamp(activeTo) };
    EndpointEvidence retiredEvidence { retiredTo != 0, 0, CaptureStamp(retiredTo) };
    bool valid = causalSeq != 0 && EndpointValid(targetEvidence.present, targetEvidence.stamp) &&
        EndpointValid(activeEvidence.present, activeEvidence.stamp) &&
        EndpointValid(retiredEvidence.present, retiredEvidence.stamp);
    AllocationToken token = 0;
    if (valid) {
        std::lock_guard<std::mutex> lock(g_registryLock);
        std::unordered_set<AllocationToken> candidates;
        if (activeTo != 0) {
            const AllocationToken active = ResolveStampLocked(targetEvidence.stamp, activeTo, true);
            if (active != 0) {
                candidates.insert(active);
            }
        }
        if (retiredTo != 0) {
            const AllocationToken retired = ResolveStampLocked(targetEvidence.stamp, retiredTo, true);
            if (retired != 0) {
                candidates.insert(retired);
            }
        }
        if (activeTo == 0 && retiredTo == 0) {
            const AllocationToken direct = ResolveStampLocked(targetEvidence.stamp, 0, false);
            if (direct != 0) {
                candidates.insert(direct);
            }
        }
        if (candidates.size() == 1) {
            token = *candidates.begin();
        } else {
            valid = false;
        }
    }
    if (!valid) {
        ContractError();
    }
    targetEvidence.token = token;
    const char* outputClass = valid ? classification : "INVALID_EVIDENCE";
#if defined(MRT_GC_UNIT_TEST_ACCESS)
    if (g_dropNextM0Write.exchange(false, std::memory_order_relaxed)) {
        return;
    }
#endif
    const uint64_t ledger = Emit(
        "m0",
        "causal_seq=%llu m0_seq=%llu exit=%s class=%s phase=%u allocation_token=%llu "
        "target.present=1 target.valid=%s target.address=%#llx target.region_start=%#llx "
        "target.region_life=%llu target.offset=%#llx active_to.present=%s active_to.valid=%s "
        "active_to.address=%#llx active_to.region_start=%#llx active_to.region_life=%llu "
        "active_to.offset=%#llx retired_to.present=%s retired_to.valid=%s retired_to.address=%#llx "
        "retired_to.region_start=%#llx retired_to.region_life=%llu retired_to.offset=%#llx",
        static_cast<unsigned long long>(causalSeq), static_cast<unsigned long long>(m0Seq), exitName,
        outputClass, static_cast<unsigned>(phase), static_cast<unsigned long long>(token),
        BoolDigit(targetEvidence.stamp.valid), static_cast<unsigned long long>(targetEvidence.stamp.address),
        static_cast<unsigned long long>(targetEvidence.stamp.regionStart),
        static_cast<unsigned long long>(targetEvidence.stamp.regionLife),
        static_cast<unsigned long long>(targetEvidence.stamp.offset), BoolDigit(activeEvidence.present),
        BoolDigit(activeEvidence.stamp.valid), static_cast<unsigned long long>(activeEvidence.stamp.address),
        static_cast<unsigned long long>(activeEvidence.stamp.regionStart),
        static_cast<unsigned long long>(activeEvidence.stamp.regionLife),
        static_cast<unsigned long long>(activeEvidence.stamp.offset), BoolDigit(retiredEvidence.present),
        BoolDigit(retiredEvidence.stamp.valid), static_cast<unsigned long long>(retiredEvidence.stamp.address),
        static_cast<unsigned long long>(retiredEvidence.stamp.regionStart),
        static_cast<unsigned long long>(retiredEvidence.stamp.regionLife),
        static_cast<unsigned long long>(retiredEvidence.stamp.offset));
    if (ledger != 0) {
        g_m0Written.fetch_add(1, std::memory_order_relaxed);
    }
}

#if defined(MRT_GC_UNIT_TEST_ACCESS)
void ResetForTest()
{
    std::lock_guard<std::mutex> registry(g_registryLock);
    std::lock_guard<std::mutex> sink(g_sinkLock);
    g_current.clear();
    g_lineage.clear();
    g_external.clear();
    g_forwardFrom.clear();
    g_nextAllocationToken = 1;
    g_nextTransition = 1;
    g_selectedExternalKey = 0;
    g_selectorSet = false;
    g_pendingExternalKey = 0;
    g_ledgerSeq = 0;
    g_causalSeq.store(0, std::memory_order_relaxed);
    g_binds.store(0, std::memory_order_relaxed);
    g_tagRejected.store(0, std::memory_order_relaxed);
    g_forwards.store(0, std::memory_order_relaxed);
    g_m0Seen.store(0, std::memory_order_relaxed);
    g_m0Written.store(0, std::memory_order_relaxed);
    g_observations.store(0, std::memory_order_relaxed);
    g_releases.store(0, std::memory_order_relaxed);
    g_interventionRegister.store(0, std::memory_order_relaxed);
    g_interventionUnregister.store(0, std::memory_order_relaxed);
    g_contractErrors.store(0, std::memory_order_relaxed);
    g_writeErrors.store(0, std::memory_order_relaxed);
    g_dropNextM0Write.store(false, std::memory_order_relaxed);
}

AllocationToken BindStampForTest(uint64_t externalKey, const ObjectStamp& stamp)
{
    std::lock_guard<std::mutex> lock(g_registryLock);
    return BindStampLocked(externalKey, stamp, nullptr);
}

AllocationToken LookupStampForTest(const ObjectStamp& stamp)
{
    std::lock_guard<std::mutex> lock(g_registryLock);
    return ResolveStampLocked(stamp, 0, false);
}

AllocationToken ExternalTokenForTest(uint64_t externalKey)
{
    std::lock_guard<std::mutex> lock(g_registryLock);
    std::unordered_map<uint64_t, AllocationToken>::const_iterator external = g_external.find(externalKey);
    return external == g_external.end() ? 0 : external->second;
}

bool ValidateEndpointForTest(bool present, const ObjectStamp& stamp)
{
    return EndpointValid(present, stamp);
}

const char* ClassifyEvidenceForTest(bool targetPresent, const ObjectStamp& target,
                                    bool consumerPresent, const ObjectStamp& consumer,
                                    bool activeToPresent, const ObjectStamp& activeTo,
                                    bool retiredToPresent, const ObjectStamp& retiredTo)
{
    return EndpointValid(targetPresent, target) && EndpointValid(consumerPresent, consumer) &&
        EndpointValid(activeToPresent, activeTo) && EndpointValid(retiredToPresent, retiredTo)
        ? "VALID" : "INVALID_EVIDENCE";
}

void DropNextM0WriteForTest()
{
    g_dropNextM0Write.store(true, std::memory_order_relaxed);
}

bool FooterValidForTest()
{
    return FooterValid();
}

TestSnapshot SnapshotForTest()
{
    return TestSnapshot { g_binds.load(std::memory_order_relaxed),
                          g_tagRejected.load(std::memory_order_relaxed),
                          g_forwards.load(std::memory_order_relaxed),
                          g_m0Seen.load(std::memory_order_relaxed),
                          g_m0Written.load(std::memory_order_relaxed),
                          g_observations.load(std::memory_order_relaxed),
                          g_releases.load(std::memory_order_relaxed),
                          g_interventionRegister.load(std::memory_order_relaxed),
                          g_interventionUnregister.load(std::memory_order_relaxed),
                          g_contractErrors.load(std::memory_order_relaxed),
                          g_writeErrors.load(std::memory_order_relaxed) };
}
#endif

} // namespace M0Correlation

extern "C" MRT_EXPORT void MRT_M0CorrTagNextAllocation(uint64_t externalKey)
{
    M0Correlation::TagNextAllocation(externalKey);
}

extern "C" MRT_EXPORT void MRT_M0CorrObserve(ObjectPtr consumedObject, uint64_t externalKey,
                                               uint64_t consumerToken, uint64_t payload0, uint64_t payload1)
{
    M0Correlation::Observe(consumedObject, externalKey, consumerToken, payload0, payload1);
}

extern "C" MRT_EXPORT void MRT_M0CorrRelease(uint64_t externalKey)
{
    M0Correlation::Release(externalKey);
}

extern "C" MRT_EXPORT void MRT_M0CorrSelectKeepLive(uint64_t externalKey)
{
    M0Correlation::SelectKeepLive(externalKey);
}

extern "C" MRT_EXPORT uint8_t MRT_M0CorrIsSelected(uint64_t externalKey)
{
    return M0Correlation::IsSelected(externalKey) ? 1u : 0u;
}

} // namespace MapleRuntime

#endif // MRT_M0_CORRELATION_EXPERIMENT
