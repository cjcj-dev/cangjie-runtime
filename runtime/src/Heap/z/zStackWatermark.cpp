// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#include "Heap/z/zStackWatermark.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zThreadLocalData.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Mutator/Mutator.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/StackFrameCursor.h"

#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
// ===== OPUS500 PROBE (test-only instrumentation, not for merge) =====
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <sys/syscall.h>
namespace MapleRuntime {
namespace {
struct Opus500Snap { std::unordered_map<uintptr_t, uintptr_t> vals; uint32_t epoch = 0; };
std::mutex g_o5Mu;
std::unordered_map<const void*, Opus500Snap> g_o5Snaps;
std::once_flag g_o5Once;
int g_o5Fd = -1;
std::atomic<uint64_t> g_o5Lines{0};
enum { C_SNAPSLOT, C_CMP, C_MUT, C_ZERO2X, C_MUTNULLED, C_NULLED, C_DEADREG, C_WORKERREG, C_UNCNULL, C_PROC, C_SNAPS, C_MUTCHANGED, C_MUTFWD, C_R4OK, C_R4WRONGSTUB, C_R4CFRAME, C_R4BELOWSP, C_R4OTHER, C_N };
const char* const g_o5Names[C_N] = {"snapslot","cmp","mut","zero2x","mutnulled","nulled","deadreg","workerreg","uncnull","procframes","snaps","mutchanged","mutfwd","r4ok","r4wrongstub","r4cframe","r4belowsp","r4other"};
std::atomic<uint64_t> g_o5C[C_N];
int O5Fd()
{
    std::call_once(g_o5Once, [] {
        const char* p = getenv("OPUS500_PROBE_LOG");
        if (p == nullptr) { return; }
        char path[512];
        snprintf(path, sizeof(path), "%s.%d", p, static_cast<int>(getpid()));
        g_o5Fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_o5Fd < 0) { return; }
        FILE* m = fopen("/proc/self/maps", "r");
        char line[1024];
        while (m != nullptr && fgets(line, sizeof(line), m) != nullptr) {
            if (strstr(line, "libcangjie-runtime") != nullptr || strstr(line, "cjcj-stage2") != nullptr) {
                dprintf(g_o5Fd, "MAPS %s", line);
            }
        }
        if (m != nullptr) { fclose(m); }
        char exe[512] = {0};
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) { dprintf(g_o5Fd, "EXE %s\n", exe); }
    });
    return g_o5Fd;
}
void O5Summary(const char* why)
{
    int fd = O5Fd();
    if (fd < 0) { return; }
    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "SUMMARY why=%s", why);
    for (int i = 0; i < C_N; ++i) {
        off += snprintf(buf + off, sizeof(buf) - off, " %s=%llu", g_o5Names[i],
                        static_cast<unsigned long long>(g_o5C[i].load(std::memory_order_relaxed)));
    }
    dprintf(fd, "%s\n", buf);
}
#define O5LOG(cap, ...) do { int _fd = O5Fd(); \
    if (_fd >= 0 && (cap) && g_o5Lines.fetch_add(1, std::memory_order_relaxed) < 6000) { dprintf(_fd, __VA_ARGS__); } } while (0)
uint64_t O5Count(int c)
{
    uint64_t v = g_o5C[c].fetch_add(1, std::memory_order_relaxed) + 1;
    if (c == C_PROC && (v & 0x7f) == 0) { O5Summary("periodic"); }
    if (c == C_SNAPS && (v & 0x7) == 0) { O5Summary("snaps"); }
    if (c == C_MUT || c == C_NULLED || c == C_UNCNULL || c == C_MUTCHANGED) { O5Summary("event"); }
    return v;
}
thread_local int g_o5Mode = 0; // 0 start,1 lazy(process_one),2 finish,3 growflush
struct O5ModeScope { int saved; explicit O5ModeScope(int m) : saved(g_o5Mode) { if (!(m == 1 && g_o5Mode >= 4)) { g_o5Mode = m; } } ~O5ModeScope() { g_o5Mode = saved; } };
int O5Tid() { return static_cast<int>(syscall(SYS_gettid)); }
bool O5On() { return O5Fd() >= 0; }
} // namespace
} // namespace MapleRuntime
extern "C" void Opus500ProbeUncNull(void* p, uintptr_t addr, uintptr_t color)
{
    using namespace MapleRuntime;
    if (!O5On()) { return; }
    uint64_t n = O5Count(C_UNCNULL);
    O5LOG(n <= 2000, "UNCNULL slot=%p before=0x%lx color=0x%lx tid=%d\n", p, static_cast<unsigned long>(addr),
          static_cast<unsigned long>(color), O5Tid());
    O5Summary("uncnull");
}
// ===== END OPUS500 PROBE header =====

#endif
namespace MapleRuntime {

// runtime/stackWatermark.cpp:44-153. The stream owns its register locations,
// and caller/callee identify the two processed frames guarding the frontier.
class StackWatermarkFramesIterator {
public:
    explicit StackWatermarkFramesIterator(StackWatermark& owner)
        : owner(owner), cursor(owner.owner.GetUnwindContext()) {}
    bool has_next() const { return !cursor.Done(); }
    uintptr_t caller() const { return callerSP; }
    uintptr_t callee() const { return calleeSP; }
    void process_one(void* context)
    {
        uintptr_t stackTarget = 0;
        while (has_next()) {
            const FrameInfo frame = *cursor.CurrentFrame();
            const bool barrier = has_barrier(frame);
            process_frame(frame, context, stackTarget);
            cursor.Advance();
            if (barrier) {
                set_watermark(frame.mFrame.GetSP());
                if (covers_stack_target(stackTarget)) { break; }
            }
        }
    }
    void process_all(void* context)
    {
        unsigned processed = 0;
        uintptr_t stackTarget = 0;
        while (has_next()) {
            const FrameInfo frame = *cursor.CurrentFrame();
            const bool barrier = has_barrier(frame);
            process_frame(frame, context, stackTarget);
            cursor.Advance();
            if (barrier) {
                set_watermark(frame.mFrame.GetSP());
                if (++processed >= 5 && covers_stack_target(stackTarget)) {
                    processed = 0;
                    owner.yield_processing();
                }
            }
        }
    }
    // continuationFreezeThaw.cpp:595-598 flushes every frame that the copy will
    // publish. yield_processing (stackWatermark.cpp:225) would let a concurrent
    // watermark write land between the healed slot and the bytes that are copied.
    void process_all_no_yield(void* context)
    {
        while (has_next()) {
            const FrameInfo frame = *cursor.CurrentFrame();
            const bool barrier = has_barrier(frame);
            owner.process(frame, cursor.RegMap(), context);
            cursor.Advance();
            if (barrier) {
                set_watermark(frame.mFrame.GetSP());
            }
        }
    }
    void rebase(intptr_t offset)
    {
        if (callerSP != 0) { callerSP += offset; }
        if (calleeSP != 0) { calleeSP += offset; }
        cursor.Rebase(offset);
    }
private:
    // HotSpot stackWatermark.cpp:96-110,205-220 protects exposed frames and
    // their callers. Cangjie value-type sret arguments can name a more distant
    // caller frame: close over those live stack pointers before exposing it.
    void process_frame(const FrameInfo& frame, void* context, uintptr_t& stackTarget)
    {
        // Root processing consumes register locations and records the caller's
        // spills. Stack pointers at this PC need the incoming register map.
        RegSlotsMap registers = cursor.RegMap();
        owner.process(frame, cursor.RegMap(), context);
        if (frame.GetFrameType() != FrameType::MANAGED) { return; }
        ElfUnloadQuiescence::ReadScope metadataReader;
        StackPtrMap pointers = StackMapBuilder(reinterpret_cast<uintptr_t>(frame.GetStartProc()),
            reinterpret_cast<uintptr_t>(frame.mFrame.GetIP()),
            reinterpret_cast<uintptr_t>(frame.mFrame.GetFA())).Build<StackPtrMap>();
        if (!pointers.IsValid()) { return; }
        StackPtrVisitor visit = [&](ObjectRef& slot) {
            const uintptr_t target = raw(slot.LoadPlain());
            if (owner.owner.IsStackAddr(target) && target > stackTarget) { stackTarget = target; }
        };
        if (!pointers.VisitReg(visit, visit, nullptr, registers)) {
            LOG(RTLOG_FATAL, "wrong stack pointer register info at %p", frame.mFrame.GetIP());
        }
        pointers.VisitSlot(visit, visit, nullptr);
    }
    bool covers_stack_target(uintptr_t stackTarget) const
    {
        return stackTarget == 0 || !has_next() || cursor.CurrentFrame()->mFrame.GetSP() > stackTarget;
    }
    static bool has_barrier(const FrameInfo& frame)
    {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
        // Other compiler targets have not yet supplied a return barrier ABI.
        return frame.GetFrameType() == FrameType::MANAGED && frame.mFrame.GetSP() != 0;
#else
        return false;
#endif
    }
    void set_watermark(uintptr_t sp)
    {
        if (!has_next()) { return; }
        if (calleeSP == 0) { calleeSP = sp; }
        else if (callerSP == 0) { callerSP = sp; }
        else { calleeSP = callerSP; callerSP = sp; }
    }
    StackWatermark& owner;
    StackFrameCursor cursor;
    uintptr_t callerSP = 0;
    uintptr_t calleeSP = 0;
};

uint32_t StackWatermark::epoch_id()
{
    return __atomic_load_n(ZPointerStoreGoodMaskLowOrderBitsAddr, __ATOMIC_ACQUIRE);
}

StackWatermark::StackWatermark(Mutator& thread) : owner(thread), state(PackState(epoch_id(), true)) {}
StackWatermark::~StackWatermark() = default;

void StackWatermark::Reset()
{
    iterator.reset();
    waterMark.store(0, std::memory_order_relaxed);
    state.store(PackState(epoch_id(), true), std::memory_order_relaxed);
}

void StackWatermark::OnStackGrow(intptr_t offset)
{
    if (offset == 0) { return; }
    if (iterator != nullptr) { iterator->rebase(offset); }
    const uintptr_t old = watermark();
    if (old != 0) { waterMark.store(old + offset, std::memory_order_release); }
}

void StackWatermark::BeginGrowFlush()
{
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    O5ModeScope o5m(3);
#endif
    lock.lock();
    if (!processing_started()) {
        start_processing_impl(nullptr);
    }
    if (!IsDone() && iterator != nullptr) {
        iterator->process_all_no_yield(nullptr);
        update_watermark();
    }
}

void StackWatermark::EndGrowFlush()
{
    lock.unlock();
}

void StackWatermark::ShiftForGrow(intptr_t offset)
{
    OnStackGrow(offset);
}

uintptr_t StackWatermark::last_processed_raw() const
{
    return iterator == nullptr ? 0 : iterator->caller();
}

void StackWatermark::update_watermark()
{
    if (iterator != nullptr && iterator->has_next()) {
        waterMark.store(iterator->callee(), std::memory_order_release);
        state.store(PackState(epoch_id(), false), std::memory_order_release);
    } else {
        waterMark.store(0, std::memory_order_release);
        state.store(PackState(epoch_id(), true), std::memory_order_release);
    }
}

void StackWatermark::start_processing_impl(void* context)
{
    iterator.reset();
    if (owner.IsManagedContext()) {
        iterator.reset(new StackWatermarkFramesIterator(*this));
        // runtime/stackWatermark.cpp:205-228: callee, caller, unwind margin.
        iterator->process_one(context);
        iterator->process_one(context);
        iterator->process_one(context);
    }
    update_watermark();
}

void StackWatermark::yield_processing()
{
    update_watermark();
    lock.unlock();
    lock.lock();
}

void StackWatermark::start_processing()
{
    if (processing_started()) { return; }
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    O5ModeScope o5m(0);
#endif
    lock.lock();
    if (!processing_started()) { start_processing_impl(nullptr); }
    lock.unlock();
}

void StackWatermark::finish_processing(void* context)
{
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    O5ModeScope o5m(2);
#endif
    lock.lock();
    if (!processing_started()) { start_processing_impl(context); }
    if (!IsDone()) {
        iterator->process_all(context);
        update_watermark();
    }
    lock.unlock();
}

void StackWatermark::process_one()
{
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    O5ModeScope o5m(1);
#endif
    lock.lock();
    if (!processing_started()) { start_processing_impl(nullptr); }
    else if (!IsDone()) {
        iterator->process_one(nullptr);
        update_watermark();
    }
    lock.unlock();
}

bool StackWatermark::is_frame_safe(const FrameInfo& frame) const
{
    if (!processing_started()) { return false; }
    if (IsDone()) { return true; }
    return frame.mFrame.GetSP() < iterator->caller();
}

void StackWatermark::ensure_safe(const FrameInfo& frame)
{
    if (IsDone(epoch_id())) { return; }
    // real_fp in HotSpot is the sender's SP, not the machine frame pointer.
    const uintptr_t senderSP = frame.CallerSP();
    const uintptr_t boundary = watermark();
    if (boundary != 0 && senderSP > boundary) { process_one(); }
}

void StackWatermark::on_safepoint() { start_processing(); }

namespace {
bool HasExposableFrame(Mutator& owner)
{
    if (!owner.IsManagedContext()) {
        return false;
    }
    const MachineFrame& top = owner.GetUnwindContext().frameInfo.mFrame;
    return top.GetFA() != nullptr && top.GetIP() != nullptr;
}
}

void StackWatermark::before_unwind()
{
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    O5ModeScope o5m(5);
#endif
    // stackWatermark.inline.hpp:86-106. Processing was started by on_safepoint
    // (javaThread.cpp:1112). A finished watermark has nothing to expose, and a
    // runtime leave has no Java frame: do not classify it.
    if (!processing_started() || IsDone() || !HasExposableFrame(owner)) {
        return;
    }
    StackFrameStream frames(&owner.GetUnwindContext());
    frames.Start();
    while (!frames.IsDone() && frames.Current().GetFrameType() != FrameType::MANAGED) { frames.Next(); }
    if (frames.IsDone()) { return; }
    frames.Next();
    while (!frames.IsDone() && frames.Current().GetFrameType() != FrameType::MANAGED) { frames.Next(); }
    if (!frames.IsDone()) { ensure_safe(frames.Current()); }
}

void StackWatermark::after_unwind()
{
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    O5ModeScope o5m(4);
#endif
    // stackWatermark.inline.hpp:109-124.
    if (!processing_started() || IsDone() || !HasExposableFrame(owner)) {
        return;
    }
    StackFrameStream frames(&owner.GetUnwindContext());
    frames.Start();
    while (!frames.IsDone() && frames.Current().GetFrameType() != FrameType::MANAGED) { frames.Next(); }
    if (!frames.IsDone()) { ensure_safe(frames.Current()); }
}

// stackWatermark.inline.hpp:70-71,127-131: on_iteration assumes processing has
// already been started by on_safepoint; a watermark that never started exposes
// nothing and has no iterator to walk. Same guard shape as before_unwind /
// after_unwind above.
void StackWatermark::on_iteration(const FrameInfo& frame)
{
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    O5ModeScope o5m(6);
#endif
    if (!processing_started() || IsDone() || !HasExposableFrame(owner)) { return; }
    ensure_safe(frame);
}

StackWatermarkProcessOopClosure::RootFunction StackWatermarkProcessOopClosure::select_function(void* context)
{
    return context == nullptr ? ZUncoloredRoot::process : reinterpret_cast<RootFunction>(context);
}
StackWatermarkProcessOopClosure::StackWatermarkProcessOopClosure(void* context, uintptr_t color)
    : function(select_function(context)), color(color) {}
void StackWatermarkProcessOopClosure::do_root(zaddress_unsafe* p) { function(p, color); }

bool ZColorWatermark::covers(const ZColorWatermark& other) const
{
    if (watermark == 0) { return true; }
    if (other.watermark == 0) { return false; }
    return watermark >= other.watermark;
}

ZStackWatermark::ZStackWatermark(Mutator& owner) : StackWatermark(owner) { Reset(); }
void ZStackWatermark::Reset()
{
    StackWatermark::Reset();
    oldWatermarks[0] = { ZPointerStoreBadMask, 1 };
    oldWatermarks[1] = {};
    oldWatermarks[2] = {};
    newest = 0;
    allocStats = TLABStatistics{};
}
uintptr_t ZStackWatermark::prev_head_color() const { return oldWatermarks[newest].color; }
uintptr_t ZStackWatermark::prev_frame_color(const FrameInfo& frame) const
{
    for (int i = newest; i >= 0; --i) {
        if (oldWatermarks[i].watermark == 0 || frame.mFrame.GetSP() <= oldWatermarks[i].watermark) {
            return oldWatermarks[i].color;
        }
    }
    LOG(RTLOG_FATAL, "Found no matching previous color for the frame");
    return 0;
}

void ZStackWatermark::save_old_watermark()
{
    const uintptr_t previousColor = GetEpoch();
    if (previousColor == prev_head_color()) { return; }
    const ZColorWatermark previous { previousColor, IsDone() ? 0 : last_processed_raw() };
    int replace = -1;
    for (int i = 0; i <= newest; ++i) {
        if (previous.covers(oldWatermarks[i])) { replace = i; break; }
    }
    newest = replace == -1 ? newest + 1 : replace;
    CHECK_DETAIL(newest < OldWatermarksMax, "Unexpected amount of old watermarks");
    oldWatermarks[newest] = previous;
}

void ZStackWatermark::process_head(void* context)
{
    StackWatermarkProcessOopClosure closure(context, prev_head_color());
    RootVisitor roots = [&](RootSlot& root) {
        owner.VisitHeapRootSlots(root, [&](RootSlot& slot) {
            closure.do_root(reinterpret_cast<zaddress_unsafe*>(&slot));
        });
    };
    owner.VisitExceptionRoots(roots);
    owner.VisitNativeFrameRoots(roots);
    zaddress_unsafe* invisible = owner.GetGCData().invisibleRoot;
    if (invisible != nullptr) { ZUncoloredRoot::process_invisible(invisible, prev_head_color()); }
}

void ZStackWatermark::start_processing_impl(void* context)
{
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    if (O5On()) {
        // OPUS500: snapshot in-frame root slot values of the whole stack at epoch start.
        Opus500Snap snap;
        snap.epoch = epoch_id();
        if (owner.IsManagedContext()) {
            StackFrameCursor cur(owner.GetUnwindContext());
            DerivedPtrVisitor noDerived = [](BasePtrType, DerivedSlot&) {};
            while (!cur.Done()) {
                const FrameInfo f = *cur.CurrentFrame();
                const uintptr_t sp = f.mFrame.GetSP();
                const uintptr_t fa = reinterpret_cast<uintptr_t>(f.mFrame.GetFA());
                RootVisitor rec = [&](RootSlot& s) {
                    const uintptr_t a = reinterpret_cast<uintptr_t>(&s);
                    if (sp != 0 && a >= sp && a < fa + 16) {
                        snap.vals[a] = raw(s.LoadPlain());
                        O5Count(C_SNAPSLOT);
                    } else if (snap.vals.find(a) == snap.vals.end()) {
                        snap.vals[a] = raw(s.LoadPlain()) | 0x1; // register-root location marker (low bit)
                    }
                };
                StackFrameCursor::ProcessFrame(f, cur.RegMap(), rec, owner, &noDerived, false);
                cur.Advance();
            }
        }
        O5Count(C_SNAPS);
        std::lock_guard<std::mutex> g(g_o5Mu);
        g_o5Snaps[this] = std::move(snap);
    }
#endif
    save_old_watermark();
    process_head(context);
    owner.GetGCData().InstallMasks(ThreadGCData::PublishedMasks());
    if ((ZGeneration::young() != nullptr && ZGeneration::young()->is_phase_mark()) ||
        (ZGeneration::old() != nullptr && ZGeneration::old()->is_phase_mark())) {
        ZThreadLocalAllocBuffer::retire(owner, allocStats);
    }
    if (owner.GetGCData().storeBarrierBuffer != nullptr) { owner.GetGCData().storeBarrierBuffer->on_new_phase(); }
    StackWatermark::start_processing_impl(context);
}

void ZStackWatermark::process(const FrameInfo& frame, RegSlotsMap& registers, void* context)
{
    StackWatermarkProcessOopClosure closure(context, prev_frame_color(frame));
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    const bool o5 = O5On();
    Opus500Snap* o5snap = nullptr;
    if (o5) {
        O5Count(C_PROC);
        std::lock_guard<std::mutex> g(g_o5Mu);
        auto it = g_o5Snaps.find(this);
        if (it != g_o5Snaps.end() && it->second.epoch == epoch_id()) { o5snap = &it->second; }
    }
    const uintptr_t o5sp = frame.mFrame.GetSP();
    const uintptr_t o5fa = reinterpret_cast<uintptr_t>(frame.mFrame.GetFA());
    const uintptr_t o5ip = reinterpret_cast<uintptr_t>(frame.mFrame.GetIP());
    const bool o5mut = o5 && Mutator::GetMutator() == &owner;
    const uintptr_t o5stub = o5mut ? reinterpret_cast<uintptr_t>(owner.GetUnwindContext().frameInfo.mFrame.GetFA()) : 0;
#endif
    RootVisitor roots = [&](RootSlot& root) {
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
        uintptr_t before = 0;
        bool o5wasMut = false;
        const uintptr_t a = reinterpret_cast<uintptr_t>(&root);
        if (o5) {
            before = raw(root.LoadPlain());
            const bool inFrame = o5sp != 0 && a >= o5sp && a < o5fa + 16;
            if (inFrame && o5snap != nullptr) {
                auto sit = o5snap->vals.find(a);
                if (sit != o5snap->vals.end()) {
                    O5Count(C_CMP);
                    if (sit->second != before) {
                        o5wasMut = true;
                        uint64_t n = O5Count(C_MUT);
                        if (before != 0) {
                            Heap& o5h = Heap::GetHeap();
                            const bool fy = o5h.GetZGeneration(ZGenerationId::young).forwarding_table().get(before) != nullptr;
                            const bool fo = o5h.GetZGeneration(ZGenerationId::old).forwarding_table().get(before) != nullptr;
                            if (fy || fo) {
                                O5Count(C_MUTFWD);
                                O5LOG(true, "MUTFWD slot=0x%lx now=0x%lx young_fwd=%d old_fwd=%d\n", static_cast<unsigned long>(a),
                                      static_cast<unsigned long>(before), fy ? 1 : 0, fo ? 1 : 0);
                            }
                        }
                        if (sit->second == 0) { O5Count(C_ZERO2X); }
                        O5LOG(n <= 2500, "MUT ip=0x%lx fa=0x%lx slot=0x%lx snap=0x%lx now=0x%lx onmut=%d mode=%d tid=%d\n",
                              static_cast<unsigned long>(o5ip), static_cast<unsigned long>(o5fa),
                              static_cast<unsigned long>(a), static_cast<unsigned long>(sit->second),
                              static_cast<unsigned long>(before), o5mut ? 1 : 0, g_o5Mode, O5Tid());
                    }
                }
            } else if (!inFrame && o5sp != 0 && a < o5sp && o5mut && g_o5Mode == 4) {
                int reg = -1;
                for (int i = 0; i < static_cast<int>(REGISTERS_COUNT); ++i) {
                    if (registers.isRecorded[i] && reinterpret_cast<uintptr_t>(registers.addrMap[i]) == a) { reg = i; break; }
                }
                const uintptr_t curSp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
                // HandleReturnSafepointStub: fa-8 rax, -16 rbx, -24 rcx, -32 rdx, -40 rdi, -48 rsi, -56 rsp, -64 r8 ... -120 r15
                static const int kStubReg[15] = {0, 3, 1, 2, 5, 4, 7, 8, 9, 10, 11, 12, 13, 14, 15}; // x86 dwarf-like ids, filled below
                (void)kStubReg;
                int cls;
                if (a < curSp) { cls = C_R4BELOWSP; }
                else if (o5stub != 0 && a < o5stub && a >= o5stub - 120) { cls = C_R4OK; }
                else if (a >= curSp && a < o5sp) { cls = C_R4CFRAME; }
                else { cls = C_R4OTHER; }
                uint64_t n = O5Count(cls);
                if (cls == C_R4OK) {
                    O5LOG(n <= 300, "R4STUB ip=0x%lx reg=%d stubslot=-%ld val=0x%lx tid=%d\n", static_cast<unsigned long>(o5ip), reg,
                          static_cast<long>(o5stub - a), static_cast<unsigned long>(before), O5Tid());
                } else {
                    O5LOG(n <= 300, "R4BAD cls=%d ip=0x%lx fa=0x%lx sp=0x%lx reg=%d slot=0x%lx val=0x%lx stubfa=0x%lx cursp=0x%lx tid=%d\n",
                          cls, static_cast<unsigned long>(o5ip), static_cast<unsigned long>(o5fa), static_cast<unsigned long>(o5sp), reg,
                          static_cast<unsigned long>(a), static_cast<unsigned long>(before), static_cast<unsigned long>(o5stub),
                          static_cast<unsigned long>(curSp), O5Tid());
                }
            } else if (!inFrame && o5sp != 0 && a < o5sp) {
                if (false) {
                    const bool inStub = o5stub != 0 && a < o5stub && a >= o5stub - 8 * 16;
                    if (!inStub) {
                        uint64_t n = O5Count(C_DEADREG);
                        O5LOG(n <= 400, "DEADREG mode=%d ip=0x%lx fa=0x%lx sp=0x%lx slot=0x%lx val=0x%lx stubfa=0x%lx tid=%d\n",
                              g_o5Mode, static_cast<unsigned long>(o5ip), static_cast<unsigned long>(o5fa), static_cast<unsigned long>(o5sp),
                              static_cast<unsigned long>(a), static_cast<unsigned long>(before),
                              static_cast<unsigned long>(o5stub), O5Tid());
                    }
                } else if (!o5mut) {
                    O5Count(C_WORKERREG);
                }
            }
        }
#endif
        owner.VisitHeapRootSlots(root, [&](RootSlot& slot) {
            closure.do_root(reinterpret_cast<zaddress_unsafe*>(&slot));
        });
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
        if (o5) {
            const uintptr_t after = raw(root.LoadPlain());
            if (o5wasMut && after != before) {
                O5Count(C_MUTCHANGED);
                O5LOG(true, "MUTCHANGED ip=0x%lx slot=0x%lx before=0x%lx after=0x%lx tid=%d\n", static_cast<unsigned long>(o5ip),
                      static_cast<unsigned long>(a), static_cast<unsigned long>(before), static_cast<unsigned long>(after), O5Tid());
            }
            if (before != 0 && after == 0) {
                uint64_t n = O5Count(C_NULLED);
                const bool inFrame = o5sp != 0 && a >= o5sp && a < o5fa + 16;
                O5LOG(n <= 1000, "NULLED mode=%d ip=0x%lx fa=0x%lx sp=0x%lx slot=0x%lx before=0x%lx inframe=%d onmut=%d stubfa=0x%lx tid=%d\n", g_o5Mode,
                      static_cast<unsigned long>(o5ip), static_cast<unsigned long>(o5fa), static_cast<unsigned long>(o5sp),
                      static_cast<unsigned long>(a), static_cast<unsigned long>(before), inFrame ? 1 : 0,
                      o5mut ? 1 : 0, static_cast<unsigned long>(o5stub), O5Tid());
                if (inFrame && o5snap != nullptr) {
                    auto sit = o5snap->vals.find(a);
                    if (sit != o5snap->vals.end() && sit->second != before) { O5Count(C_MUTNULLED); }
                }
                int reg = -1;
                for (int i = 0; i < static_cast<int>(REGISTERS_COUNT); ++i) {
                    if (registers.isRecorded[i] && reinterpret_cast<uintptr_t>(registers.addrMap[i]) == a) { reg = i; break; }
                }
                bool snapHas = false; uintptr_t snapVal = 0;
                if (o5snap != nullptr) {
                    auto sit = o5snap->vals.find(a);
                    if (sit != o5snap->vals.end()) { snapHas = true; snapVal = sit->second; }
                }
                O5LOG(true, "NULLCTX reg=%d framecolor=0x%lx loadgood=0x%lx storegood=0x%lx epoch=0x%x newest=%d "
                      "ow0=0x%lx@0x%lx ow1=0x%lx@0x%lx ow2=0x%lx@0x%lx snapHas=%d snapVal=0x%lx cursp=0x%lx wm=0x%lx\n",
                      reg, static_cast<unsigned long>(prev_frame_color(frame)), static_cast<unsigned long>(ZPointerLoadGoodMask),
                      static_cast<unsigned long>(ZPointerStoreGoodMask), epoch_id(), newest,
                      static_cast<unsigned long>(oldWatermarks[0].color), static_cast<unsigned long>(oldWatermarks[0].watermark),
                      static_cast<unsigned long>(oldWatermarks[1].color), static_cast<unsigned long>(oldWatermarks[1].watermark),
                      static_cast<unsigned long>(oldWatermarks[2].color), static_cast<unsigned long>(oldWatermarks[2].watermark),
                      snapHas ? 1 : 0, static_cast<unsigned long>(snapVal),
                      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(__builtin_frame_address(0))),
                      static_cast<unsigned long>(watermark()));
            }
        }
#endif
    };
    DerivedPtrVisitor derived = Mutator::MakeDerivedRootVisitor(roots);
    StackFrameCursor::ProcessFrame(frame, registers, roots, owner, &derived, false);
}

void ZStackWatermark::ShiftForGrow(intptr_t offset)
{
    StackWatermark::OnStackGrow(offset);
    for (int i = 0; i <= newest; ++i) {
        if (oldWatermarks[i].watermark > 1) { oldWatermarks[i].watermark += offset; }
    }
}

void ZStackWatermark::OnStackGrow(intptr_t offset)
{
    std::lock_guard<std::mutex> guard(lock);
    ShiftForGrow(offset);
}

void StackWatermarkSet::on_safepoint(Mutator& mutator) { mutator.GetStackWatermark().on_safepoint(); }
void StackWatermarkSet::start_processing(Mutator& mutator) { mutator.GetStackWatermark().start_processing(); }
void StackWatermarkSet::finish_processing(Mutator& mutator, void* context)
{
    mutator.GetStackWatermark().finish_processing(context);
}
void StackWatermarkSet::before_unwind(Mutator& mutator)
{
    mutator.GetStackWatermark().before_unwind();
    UpdatePollValues(ThreadLocal::GetThreadLocalData());
}
void StackWatermarkSet::after_unwind(Mutator& mutator)
{
    mutator.GetStackWatermark().after_unwind();
    UpdatePollValues(ThreadLocal::GetThreadLocalData());
}
void StackWatermarkSet::on_iteration(Mutator& mutator, const FrameInfo& frame)
{
    mutator.GetStackWatermark().on_iteration(frame);
}
uintptr_t StackWatermarkSet::lowest_watermark(Mutator& mutator) { return mutator.GetStackWatermark().watermark(); }
} // namespace MapleRuntime
