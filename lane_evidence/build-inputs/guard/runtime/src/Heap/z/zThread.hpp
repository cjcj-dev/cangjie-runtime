// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_Z_ZTHREAD_HPP
#define MRT_GC_Z_ZTHREAD_HPP

#include "Heap/z/concurrentGCThread.hpp"

namespace MapleRuntime {
// zThread.hpp:31-40. A ZThread is a ConcurrentGCThread with some ZGC-specific
// handling of GC shutdown. Director, the two drivers, Stat and Uncommitter
// derive from it and are started/stopped through one protocol.
class ZThread : public ConcurrentGCThread {
private:
    virtual void run_service();
    virtual void stop_service();

public:
    virtual void run_thread() = 0;
    virtual void terminate() = 0;
};
} // namespace MapleRuntime
#endif // MRT_GC_Z_ZTHREAD_HPP
