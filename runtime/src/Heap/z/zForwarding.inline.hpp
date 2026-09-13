// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include "Heap/z/zForwarding.hpp"

namespace MapleRuntime {
inline bool ZForwarding::claim()
{ return ZForwardingLife::claim(_claimed); }
}

namespace MapleRuntime {
inline bool ZForwarding::is_claimed() const
{ return _claimed.load(std::memory_order_acquire); }
}

namespace MapleRuntime {
inline bool ZForwarding::in_place() const
{ return _in_place.load(std::memory_order_acquire); }
}

namespace MapleRuntime {
inline void ZForwarding::set_in_place()
{ _in_place.store(true, std::memory_order_release); }
}

namespace MapleRuntime {
inline bool ZForwarding::retain_page()
{
        return ZForwardingLife::retain_page(_ref_count, [this] { ZForwardingLife::WaitPageDone(this); });
    }
}

namespace MapleRuntime {
inline void ZForwarding::release_page()
{
        int32_t count = _ref_count.load(std::memory_order_relaxed);
        for (;;) {
            CHECK(count != 0);
            const int32_t next = count > 0 ? count - 1 : count + 1;
            if (_ref_count.compare_exchange_weak(count, next, std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
                if (next == 0 || next == -1) {
                    std::lock_guard<std::mutex> lock(_ref_lock);
                    _ref_changed.notify_all();
                }
                return;
            }
        }
    }
}

namespace MapleRuntime {
inline void ZForwarding::detach_page()
{
        std::unique_lock<std::mutex> lock(_ref_lock);
        _ref_changed.wait(lock, [this] { return _ref_count.load(std::memory_order_acquire) == 0; });
    }
}

namespace MapleRuntime {
inline void ZForwarding::mark_done()
{ ZForwardingLife::mark_done(_done); }
}

namespace MapleRuntime {
inline bool ZForwarding::is_done() const
{ return ZForwardingLife::is_done(_done); }
}

namespace MapleRuntime {
inline void ZForwarding::in_place_relocation_claim_page()
{
        int32_t count = _ref_count.load(std::memory_order_relaxed);
        do {
            CHECK(count > 0);
        } while (!_ref_count.compare_exchange_weak(count, -count, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed));
        std::unique_lock<std::mutex> lock(_ref_lock);
        _ref_changed.wait(lock, [this] { return _ref_count.load(std::memory_order_acquire) == -1; });
    }
}
