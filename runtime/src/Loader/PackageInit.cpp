// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Loader/PackageInit.h"

#include <atomic>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "Base/Panic.h"
#include "Base/ImmortalWrapper.h"
#include "LoaderManager.h"
#if defined(_WIN64)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <link.h>
#endif
#include "Common/ScopedObjectAccess.h"
#include "schedule.h"
#include "waitqueue.h"

namespace MapleRuntime {
namespace {
enum class InitStatus { Uninitialized, Initializing, Initialized, Failed };
}

struct PackageInitState {
    const void* package;
    const void* unit;
    uint32_t phase;
    std::atomic<InitStatus> status { InitStatus::Uninitialized };
    CJThreadHandle owner { nullptr };
    uint32_t failure { 0 };
    Waitqueue waiters {};
    std::unique_ptr<ElfUnloadQuiescence::PendingTask> admission;

    PackageInitState(const void* package, const void* unit, uint32_t phase)
        : package(package), unit(unit), phase(phase)
    {
        CHECK_DETAIL(WaitqueueNew(&waiters) == 0, "package initializer waitqueue creation failed");
    }
    ~PackageInitState()
    {
        CHECK_DETAIL(status.load(std::memory_order_relaxed) != InitStatus::Initializing,
                     "package initializer must finish before image purge");
        CHECK_DETAIL(WaitqueueDelete(&waiters) == 0, "package initializer waitqueue deletion failed");
    }
};

namespace {
// The graph is keyed by logical CJThread handles, never OS TLS. Lock order:
// admission -> short catalog lookup -> graph; no lock spans managed body/park.
// Waitqueue callbacks read only the atomic predicate (no graph-lock inversion).
struct InitCoordinator {
    std::mutex mutex;
    std::unordered_map<CJThreadHandle, PackageInitState*> waitingOn;
    std::unordered_map<uintptr_t, PackageInitState*> tokens;
    uintptr_t nextToken { 1 };
};

InitCoordinator& Coordinator()
{
    static ImmortalWrapper<InitCoordinator> coordinator;
    return *coordinator;
}

bool IsCodeAddress(Uptr address)
{
#if defined(_WIN64)
    MEMORY_BASIC_INFORMATION info {};
    constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != 0 &&
        info.State == MEM_COMMIT && (info.Protect & executable) != 0;
#elif defined(__APPLE__)
    mach_vm_address_t region = address;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto rc = mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL) { mach_port_deallocate(mach_task_self(), object); }
    return rc == KERN_SUCCESS && address >= region && address - region < size &&
        (info.protection & VM_PROT_EXECUTE) != 0;
#else
    struct Query { Uptr address; bool executable; } query { address, false };
    dl_iterate_phdr([](dl_phdr_info* image, size_t, void* context) {
        auto& q = *static_cast<Query*>(context);
        for (size_t i = 0; i != image->dlpi_phnum; ++i) {
            const auto& segment = image->dlpi_phdr[i];
            const Uptr start = image->dlpi_addr + segment.p_vaddr;
            if (segment.p_type == PT_LOAD && (segment.p_flags & PF_X) != 0 &&
                q.address >= start && q.address - start < segment.p_memsz) {
                q.executable = true;
                return 1;
            }
        }
        return 0;
    }, &query);
    return query.executable;
#endif
}

BaseFile* ResolvePackageFile(const void* package, const void* unit)
{
    const Uptr entry = reinterpret_cast<Uptr>(package);
    const Uptr unitEntry = reinterpret_cast<Uptr>(unit);
    if (!IsCodeAddress(entry) || !IsCodeAddress(unitEntry)) { return nullptr; }
    BaseFile* exact = nullptr;
    BaseFile* imageOwner = nullptr;
    size_t exactCount = 0;
    size_t imageCount = 0;
    LoaderManager::GetInstance()->GetLoader()->VisitBaseFile([&](BaseFile* file) {
        if (!file->IsRegistered()) { return false; }
        const Uptr metadata = file->GetFileMetaAddr();
        if (!ElfUnloadQuiescence::IsAddressInImage(entry, metadata) ||
            !ElfUnloadQuiescence::IsAddressInImage(unitEntry, metadata)) { return false; }
        imageOwner = file;
        ++imageCount;
        std::vector<Uptr> entries;
        file->GetGlobalInitFunc(entries);
        if (std::find(entries.begin(), entries.end(), entry) != entries.end()) {
            exact = file;
            ++exactCount;
        }
        return false;
    });
    // Exact initializer identity disambiguates multiple metadata owners in an
    // image. The code/image fallback is valid only for one registered owner.
    return exactCount == 1 ? exact : (exactCount == 0 && imageCount == 1 ? imageOwner : nullptr);
}

bool Finished(void* argument)
{
    return static_cast<PackageInitState*>(argument)->status.load(std::memory_order_acquire) !=
        InitStatus::Initializing;
}

bool HasCycle(InitCoordinator& graph, CJThreadHandle caller, PackageInitState& target)
{
    CJThreadHandle owner = target.owner;
    while (owner != nullptr) {
        if (owner == caller) {
            return true;
        }
        auto edge = graph.waitingOn.find(owner);
        if (edge == graph.waitingOn.end() ||
            edge->second->status.load(std::memory_order_acquire) != InitStatus::Initializing) {
            return false;
        }
        owner = edge->second->owner;
    }
    return false;
}

// HotSpot instanceKlass.cpp:1653, set_initialization_state_and_notify:
// publish the terminal state before waking every waiter. Fail does not touch
// the managed pending exception and performs no allocation.
void Publish(PackageInitState& state, InitStatus status, uint32_t failure) noexcept
{
    state.failure = failure;
    state.owner = nullptr;
    state.status.store(status, std::memory_order_release);
    const int rc = WaitqueueWakeAll(&state.waiters, nullptr, nullptr);
    CHECK_DETAIL(rc == 0 || rc == ERRNO_QUEUE_IS_EMPTY, "package initializer wake failed: %d", rc);
    // Woken callers retain their own admission until their acquire recheck.
    state.admission.reset();
}

void FinishToken(void* token, InitStatus status, uint32_t failure) noexcept
{
    ScopedEnterSaferegion safe(false);
    auto& graph = Coordinator();
    std::lock_guard<std::mutex> lock(graph.mutex);
    auto found = graph.tokens.find(reinterpret_cast<uintptr_t>(token));
    CHECK_DETAIL(found != graph.tokens.end(), "package initializer token is not active");
    PackageInitState& state = *found->second;
    CHECK_DETAIL(state.owner == CJThreadGetHandle() && state.owner != nullptr,
                 "package initializer token belongs to another logical CJThread");
    Publish(state, status, failure);
    graph.tokens.erase(found);
}
} // namespace

PackageInitTable::PackageInitTable() = default;
PackageInitTable::~PackageInitTable() = default;

PackageInitResult PackageInitTable::Begin(const void* package, const void* unit, uint32_t phase, void** token,
                                         std::unique_ptr<ElfUnloadQuiescence::PendingTask> admission)
{
    auto& graph = Coordinator();
    const CJThreadHandle caller = CJThreadGetHandle();
    std::unique_lock<std::mutex> lock(graph.mutex);
    PackageInitState* state = nullptr;
    for (const auto& candidate : units) {
        if (candidate->package == package && candidate->unit == unit && candidate->phase == phase) {
            state = candidate.get();
            break;
        }
    }
    if (state == nullptr) {
        units.emplace_back(new PackageInitState(package, unit, phase));
        state = units.back().get();
    }

    // HotSpot instanceKlass.cpp:1441-1498: wait, reentrant, ready, failed,
    // then grant execution. Cache reentry is a distinct result, not Ready.
    while (state->status.load(std::memory_order_acquire) == InitStatus::Initializing && state->owner != caller) {
        if (HasCycle(graph, caller, *state)) {
            return PackageInitResult::Cycle;
        }
        graph.waitingOn[caller] = state;
        lock.unlock();
        const int rc = WaitqueuePark(&state->waiters, LLONG_MAX, Finished, state, false);
        lock.lock();
        graph.waitingOn.erase(caller);
        if (rc != 0 && rc != ERRNO_CALLBACK_RETURN_TRUE) {
            return PackageInitResult::Unavailable;
        }
    }
    switch (state->status.load(std::memory_order_acquire)) {
        case InitStatus::Initializing:
            return PackageInitResult::Reentrant;
        case InitStatus::Initialized:
            return PackageInitResult::Ready;
        case InitStatus::Failed:
            return PackageInitResult::Failed;
        case InitStatus::Uninitialized:
            break;
    }
    // Monotonic opaque identities cannot alias a released token after image
    // reload, even if both BaseFile and state storage reuse their old addresses.
    CHECK_DETAIL(graph.nextToken != std::numeric_limits<uintptr_t>::max(), "package initializer token overflow");
    const uintptr_t identity = graph.nextToken++;
    graph.tokens.emplace(identity, state);
    state->admission = std::move(admission);
    state->owner = caller;
    state->status.store(InitStatus::Initializing, std::memory_order_relaxed);
    *token = reinterpret_cast<void*>(identity);
    return PackageInitResult::Execute;
}

void PackageInitTable::Complete(void* token) noexcept
{
    FinishToken(token, InitStatus::Initialized, 0);
}

void PackageInitTable::Fail(void* token, uint32_t failure) noexcept
{
    FinishToken(token, InitStatus::Failed, failure);
}

void PackageInitTable::OwnerExit() noexcept
{
    const CJThreadHandle caller = CJThreadGetHandle();
    if (caller == nullptr) {
        return;
    }
    ScopedEnterSaferegion safe(false);
    auto& graph = Coordinator();
    std::lock_guard<std::mutex> lock(graph.mutex);
    for (auto it = graph.tokens.begin(); it != graph.tokens.end();) {
        if (it->second->owner == caller) {
            Publish(*it->second, InitStatus::Failed, static_cast<uint32_t>(PackageInitFailure::OwnerExit));
            it = graph.tokens.erase(it);
        } else {
            ++it;
        }
    }
    graph.waitingOn.erase(caller);
}

extern "C" uint32_t MCC_PackageInitBegin(const void* packageEntry, const void* unitEntry,
                                         uint32_t phase, void** ownerToken)
{
    if (ownerToken == nullptr) { return static_cast<uint32_t>(PackageInitResult::Unavailable); }
    *ownerToken = nullptr;
    if (packageEntry == nullptr || unitEntry == nullptr || phase > 1 ||
        Runtime::CurrentRef() == nullptr || CJThreadGetHandle() == nullptr || Mutator::GetMutator() == nullptr) {
        return static_cast<uint32_t>(PackageInitResult::Unavailable);
    }
    ScopedEnterSaferegion safe(false);
    BaseFile* file = nullptr;
    std::unique_ptr<ElfUnloadQuiescence::PendingTask> pending;
    {
        ElfUnloadQuiescence::SharedTaskAdmissionScope admission;
        ElfUnloadQuiescence::ReadScope reader;
        file = ResolvePackageFile(packageEntry, unitEntry);
        if (file == nullptr) { return static_cast<uint32_t>(PackageInitResult::Unavailable); }
        pending = std::make_unique<ElfUnloadQuiescence::PendingTask>(reinterpret_cast<Uptr>(packageEntry), admission);
    }
    return static_cast<uint32_t>(file->GetPackageInitTable().Begin(packageEntry, unitEntry, phase,
                                                                 ownerToken, std::move(pending)));
}

extern "C" void MCC_PackageInitComplete(void* ownerToken) noexcept
{
    PackageInitTable::Complete(ownerToken);
}

extern "C" void MCC_PackageInitFail(void* ownerToken, uint32_t failureCode) noexcept
{
    PackageInitTable::Fail(ownerToken, failureCode);
}

extern "C" [[noreturn]] void MCC_PackageInitAbort(const void* packageEntry, const void* unitEntry,
                                                uint32_t phase, uint32_t beginResult) noexcept
{
    // This path is called before any dependent String cache can be used.
    // Keep diagnostics native; do not construct a managed exception or String.
    std::fprintf(stderr, "package cache initialization failed: package=%p unit=%p phase=%u result=%u\n",
                 packageEntry, unitEntry, phase, beginResult);
    std::fflush(stderr);
    std::_Exit(70);
}
} // namespace MapleRuntime
