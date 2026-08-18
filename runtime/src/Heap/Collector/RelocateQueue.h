#ifndef MRT_RELOCATE_QUEUE_H
#define MRT_RELOCATE_QUEUE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace MapleRuntime {

class RegionInfo;

// zRelocate.cpp:134-151. Key+done so tests need no RegionInfo.
class RelocateWaitCore {
public:
    using HelpFn = void (*)(void*);

    void add_and_wait(void* key, const std::atomic<bool>& done);
    void* poll_and_claim();
    void on_done();
    void SetHelp(HelpFn help) { help_ = help; }
    void Reset();

    uint64_t Enqueued() const { return enqueued_.load(std::memory_order_relaxed); }
    uint64_t Claimed() const { return claimed_.load(std::memory_order_relaxed); }
    uint64_t Waited() const { return waited_.load(std::memory_order_relaxed); }

    static constexpr bool kWaitUsesConditionNotYield = true;

private:
    struct Item {
        void* key = nullptr;
        const std::atomic<bool>* done = nullptr;
        bool claimed = false;
    };

    bool isDone(const Item& it) const
    {
        return it.done != nullptr && it.done->load(std::memory_order_acquire);
    }

    bool pruneLocked();
    void* claimLocked();

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Item> queue_;
    HelpFn help_ = nullptr;
    std::atomic<uint64_t> enqueued_{ 0 };
    std::atomic<uint64_t> claimed_{ 0 };
    std::atomic<uint64_t> waited_{ 0 };
    std::atomic<int> attention_{ 0 };
};

class RelocateQueue {
public:
    using HelpFn = void (*)(RegionInfo*);

    static RelocateQueue& Instance()
    {
        static RelocateQueue q;
        return q;
    }

    void SetHelp(HelpFn help);
    void add_and_wait(RegionInfo* forwarding);
    RegionInfo* poll_and_claim();
    void OnDone(RegionInfo* forwarding);
    RelocateWaitCore& Core() { return core_; }
    static void EnsureHelpInstalled();

private:
    RelocateWaitCore core_;
    HelpFn help_ = nullptr;
};

} // namespace MapleRuntime

#endif
