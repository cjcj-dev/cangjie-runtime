// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zValue.hpp:24-158
#pragma once
#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
//
// Storage
//

template <typename S>
class ZValueStorage {
private:
    static uintptr_t _top;
    static uintptr_t _end;

public:
    static const size_t Offset = 4 * 1024;

    static uintptr_t alloc(size_t size);
};

class ZContendedStorage : public ZValueStorage<ZContendedStorage> {
public:
    static size_t alignment();
    static uint32_t count();
    static uint32_t id();
};

class ZPerCPUStorage : public ZValueStorage<ZPerCPUStorage> {
public:
    static size_t alignment();
    static uint32_t count();
    static uint32_t id();
};

class ZPerNUMAStorage : public ZValueStorage<ZPerNUMAStorage> {
public:
    static size_t alignment();
    static uint32_t count();
    static uint32_t id();
};

class ZPerWorkerStorage : public ZValueStorage<ZPerWorkerStorage> {
public:
    static size_t alignment();
    static uint32_t count();
    static uint32_t id();
};

//
// Value
//

struct ZValueIdTagType {};

template <typename S, typename T>
class ZValue {
private:
    const uintptr_t _addr;
    const uint32_t _count;

    uintptr_t value_addr(uint32_t value_id) const;

public:
    ZValue();
    ZValue(const T& value);
    template <typename... Args>
    ZValue(ZValueIdTagType, Args&&... args);

    const T* addr(uint32_t value_id = S::id()) const;
    T* addr(uint32_t value_id = S::id());

    const T& get(uint32_t value_id = S::id()) const;
    T& get(uint32_t value_id = S::id());

    void set(const T& value, uint32_t value_id = S::id());
    void set_all(const T& value);

    uint32_t count() const;
};

template <typename T> using ZContended = ZValue<ZContendedStorage, T>;
template <typename T> using ZPerCPU = ZValue<ZPerCPUStorage, T>;
template <typename T> using ZPerNUMA = ZValue<ZPerNUMAStorage, T>;
template <typename T> using ZPerWorker = ZValue<ZPerWorkerStorage, T>;

//
// Iterator
//

template<typename S, typename T>
class ZValueConstIterator;

template <typename S, typename T>
class ZValueIterator {
    friend class ZValueConstIterator<S, T>;

private:
    ZValue<S, T>* const _value;
    uint32_t            _value_id;

public:
    ZValueIterator(ZValue<S, T>* value);
    ZValueIterator(const ZValueIterator&) = default;

    bool next(T** value);
    bool next(T** value, uint32_t* value_id);
};

template <typename T> using ZPerCPUIterator = ZValueIterator<ZPerCPUStorage, T>;
template <typename T> using ZPerNUMAIterator = ZValueIterator<ZPerNUMAStorage, T>;
template <typename T> using ZPerWorkerIterator = ZValueIterator<ZPerWorkerStorage, T>;

template <typename S, typename T>
class ZValueConstIterator {
private:
    const ZValue<S, T>* const _value;
    uint32_t                  _value_id;

public:
    ZValueConstIterator(const ZValue<S, T>* value);
    ZValueConstIterator(const ZValueIterator<S, T>& other);
    ZValueConstIterator(const ZValueConstIterator&) = default;

    bool next(const T** value);
};

template <typename T> using ZPerCPUConstIterator = ZValueConstIterator<ZPerCPUStorage, T>;
template <typename T> using ZPerNUMAConstIterator = ZValueConstIterator<ZPerNUMAStorage, T>;
template <typename T> using ZPerWorkerConstIterator = ZValueConstIterator<ZPerWorkerStorage, T>;
} // namespace MapleRuntime
