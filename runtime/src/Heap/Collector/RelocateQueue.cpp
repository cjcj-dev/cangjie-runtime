#include "Heap/Collector/RelocateQueue.h"

#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {

void RelocateWaitCore::Reset()
{
    std::lock_guard<std::mutex> g(mu_);
    queue_.clear();
    help_ = nullptr;
    enqueued_.store(0, std::memory_order_relaxed);
    claimed_.store(0, std::memory_order_relaxed);
    waited_.store(0, std::memory_order_relaxed);
    attention_.store(0, std::memory_order_relaxed);
}

bool RelocateWaitCore::pruneLocked()
{
    bool any = false;
    for (size_t i = 0; i < queue_.size();) {
        if (queue_[i].key == nullptr || isDone(queue_[i])) {
            any = true;
            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    if (queue_.empty()) {
        attention_.store(0, std::memory_order_release);
    }
    return any;
}

void* RelocateWaitCore::claimLocked()
{
    for (Item& it : queue_) {
        if (it.key == nullptr || isDone(it) || it.claimed) {
            continue;
        }
        it.claimed = true;
        claimed_.fetch_add(1, std::memory_order_relaxed);
        return it.key;
    }
    return nullptr;
}

void RelocateWaitCore::add_and_wait(void* key, const std::atomic<bool>& done)
{
    if (key == nullptr || done.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock<std::mutex> lock(mu_);
    if (done.load(std::memory_order_acquire)) {
        return;
    }
    bool present = false;
    for (const Item& it : queue_) {
        if (it.key == key) {
            present = true;
            break;
        }
    }
    if (!present) {
        queue_.push_back(Item{ key, &done, false });
        enqueued_.fetch_add(1, std::memory_order_relaxed);
        if (queue_.size() == 1) {
            attention_.store(1, std::memory_order_release);
            cv_.notify_all();
        }
    }
    waited_.fetch_add(1, std::memory_order_relaxed);
    while (!done.load(std::memory_order_acquire)) {
        void* mine = nullptr;
        if (help_ != nullptr) {
            mine = claimLocked();
        }
        if (mine != nullptr) {
            HelpFn help = help_;
            lock.unlock();
            help(mine);
            lock.lock();
            continue;
        }
        cv_.wait(lock);
    }
}

void* RelocateWaitCore::poll_and_claim()
{
    if (attention_.load(std::memory_order_acquire) == 0) {
        return nullptr;
    }
    std::lock_guard<std::mutex> g(mu_);
    if (pruneLocked()) {
        cv_.notify_all();
    }
    return claimLocked();
}

void RelocateWaitCore::on_done()
{
    std::lock_guard<std::mutex> g(mu_);
    pruneLocked();
    cv_.notify_all();
}

static RelocateWaitCore* g_bound = nullptr;
static RelocateQueue::HelpFn g_regionHelp = nullptr;

static void CoreHelp(void* key)
{
    if (g_regionHelp != nullptr) {
        g_regionHelp(static_cast<RegionInfo*>(key));
    }
}

void RelocateQueue::SetHelp(HelpFn help)
{
    help_ = help;
    g_regionHelp = help;
    g_bound = &core_;
    core_.SetHelp(help != nullptr ? &CoreHelp : nullptr);
}

void RelocateQueue::add_and_wait(RegionInfo* forwarding)
{
    if (forwarding == nullptr) {
        return;
    }
    core_.add_and_wait(forwarding, forwarding->ForwardingDoneFlag());
}

RegionInfo* RelocateQueue::poll_and_claim()
{
    return static_cast<RegionInfo*>(core_.poll_and_claim());
}

void RelocateQueue::OnDone(RegionInfo* forwarding)
{
    (void)forwarding;
    core_.on_done();
}

} // namespace MapleRuntime
