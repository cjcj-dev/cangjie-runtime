// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <atomic>
#include <cstdint>

namespace MapleRuntime {
// gc/shared/gcId.cpp:79-88: collection context belongs to the executing
// thread. Worker tasks carry the submitting thread's ID into worker scopes.
class GCIdMark {
public:
    GCIdMark() : GCIdMark(Create()) {}
    explicit GCIdMark(uint64_t id) : previous(Current()) { CurrentSlot() = id; }
    ~GCIdMark() { CurrentSlot() = previous; }
    GCIdMark(const GCIdMark&) = delete;
    GCIdMark& operator=(const GCIdMark&) = delete;
    static uint64_t Current() { return CurrentSlot(); }
private:
    static uint64_t Create()
    {
        static std::atomic<uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    static uint64_t& CurrentSlot()
    {
        static thread_local uint64_t id = 0;
        return id;
    }
    const uint64_t previous;
};

// zGCIdPrinter.cpp:34-49,64-104: minor and major registrations coexist;
// the major's young and old phases retain the same collection ID.
class ZGCIdPrinter {
public:
    static char Tag(uint64_t id)
    {
        if (id == 0) return '-';
        if (id == MinorId().load(std::memory_order_acquire)) return 'y';
        if (id == MajorId().load(std::memory_order_acquire)) return MajorTag().load(std::memory_order_acquire);
        return '-';
    }
    static void SetMinorId(uint64_t id) { MinorId().store(id, std::memory_order_release); }
    static void SetMajorId(uint64_t id, char tag)
    {
        MajorTag().store(tag, std::memory_order_release);
        MajorId().store(id, std::memory_order_release);
    }
private:
    static std::atomic<uint64_t>& MinorId() { static std::atomic<uint64_t> id{0}; return id; }
    static std::atomic<uint64_t>& MajorId() { static std::atomic<uint64_t> id{0}; return id; }
    static std::atomic<char>& MajorTag() { static std::atomic<char> tag{'-'}; return tag; }
};

class ZGCIdMinor {
public:
    explicit ZGCIdMinor(uint64_t id) { ZGCIdPrinter::SetMinorId(id); }
    ~ZGCIdMinor() { ZGCIdPrinter::SetMinorId(0); }
    ZGCIdMinor(const ZGCIdMinor&) = delete;
    ZGCIdMinor& operator=(const ZGCIdMinor&) = delete;
};

class ZGCIdMajor {
public:
    ZGCIdMajor(uint64_t id, char tag) { ZGCIdPrinter::SetMajorId(id, tag); }
    ~ZGCIdMajor() { ZGCIdPrinter::SetMajorId(0, '-'); }
    ZGCIdMajor(const ZGCIdMajor&) = delete;
    ZGCIdMajor& operator=(const ZGCIdMajor&) = delete;
};
}
