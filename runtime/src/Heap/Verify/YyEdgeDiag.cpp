#include "Heap/Verify/YyEdgeDiag.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>

#include "Base/Log.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/MClass.inline.h"

namespace MapleRuntime {
namespace YyEdgeDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        return EnvIsOne("MRT_GCV2_YYEDGE") || DiagGate::TokenOn("yyedge");
    }();
    return on;
}

std::atomic<uint64_t> g_y2yN{ 0 };
std::atomic<uint64_t> g_y2yArrayListOff8{ 0 };
std::atomic<uint64_t> g_y2yHashMapBucket{ 0 };
std::atomic<uint64_t> g_y2yOther{ 0 };
std::atomic<uint64_t> g_publishN{ 0 };
std::atomic<bool> g_healthOnce{ false };
std::atomic<bool> g_atexitOnce{ false };

std::mutex g_vecLock;
std::unordered_set<BaseObject*> g_thisVec;
std::unordered_set<BaseObject*> g_prevVec;

void HealthOnce()
{
    bool expected = false;
    if (g_healthOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        LOG(RTLOG_ERROR, "[GCV2][yyedge] health probe_live=1 env=MRT_GCV2_YYEDGE=1");
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

bool NameHas(const char* name, const char* needle)
{
    return name != nullptr && std::strstr(name, needle) != nullptr;
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void NoteYoungToYoung(BaseObject* holder, MAddress fieldAddress, BaseObject* ref)
{
    if (!GateOn()) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    g_y2yN.fetch_add(1, std::memory_order_relaxed);
    (void)ref;
    if (holder == nullptr || !Heap::IsHeapAddress(holder)) {
        g_y2yOther.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    TypeInfo* tip = holder->GetTypeInfo();
    const char* name = (tip == nullptr) ? nullptr : tip->GetName();
    size_t off = 0;
    if (Heap::IsHeapAddress(fieldAddress)) {
        off = static_cast<size_t>(fieldAddress - reinterpret_cast<MAddress>(holder));
    }
    if (NameHas(name, "ArrayList") && off == 8) {
        g_y2yArrayListOff8.fetch_add(1, std::memory_order_relaxed);
    } else if (NameHas(name, "HashMapEntry") || NameHas(name, "RawArray")) {
        g_y2yHashMapBucket.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_y2yOther.fetch_add(1, std::memory_order_relaxed);
    }
}

void PublishProductVec(const std::vector<BaseObject*>& reachableVec)
{
    if (!GateOn()) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    std::lock_guard<std::mutex> guard(g_vecLock);
    g_prevVec.swap(g_thisVec);
    g_thisVec.clear();
    g_thisVec.reserve(reachableVec.size());
    for (BaseObject* object : reachableVec) {
        g_thisVec.insert(object);
    }
    g_publishN.fetch_add(1, std::memory_order_relaxed);
}

bool HolderInThisProductVec(BaseObject* holder)
{
    if (!GateOn() || holder == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(g_vecLock);
    return g_thisVec.count(holder) != 0;
}

bool HolderInPrevProductVec(BaseObject* holder)
{
    if (!GateOn() || holder == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(g_vecLock);
    return g_prevVec.count(holder) != 0;
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    size_t thisN = 0;
    size_t prevN = 0;
    {
        std::lock_guard<std::mutex> guard(g_vecLock);
        thisN = g_thisVec.size();
        prevN = g_prevVec.size();
    }
    LOG(RTLOG_ERROR,
        "[GCV2][yyedge] report point=%s y2yN=%llu y2yArrayListOff8=%llu y2yHashMapOrRaw=%llu "
        "y2yOther=%llu publishN=%llu thisVec=%zu prevVec=%zu",
        point == nullptr ? "?" : point,
        static_cast<unsigned long long>(g_y2yN.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_y2yArrayListOff8.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_y2yHashMapBucket.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_y2yOther.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_publishN.load(std::memory_order_relaxed)), thisN, prevN);
}

} // namespace YyEdgeDiag
} // namespace MapleRuntime
