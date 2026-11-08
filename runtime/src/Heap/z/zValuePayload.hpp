#ifndef MRT_Z_VALUE_PAYLOAD_HPP
#define MRT_Z_VALUE_PAYLOAD_HPP

#include "Common/BaseObject.h"
#include <algorithm>
#include <cstddef>
#include <vector>

namespace MapleRuntime {
// Cangjie values can be headerless or stack-resident. Their payload describes
// storage separately from its oop map (ZGC oops/valuePayload.hpp).
class ValuePayload {
public:
    // ZGC oops/valuePayload.hpp:44-90 keeps no oop list; zBarrierSet.inline.hpp:
    // 494-513 walks the klass oop maps in place. Cangjie layouts are GCTib or
    // per-object bitmaps, so the ascending slot offsets are materialized per
    // payload. They stay inline up to INLINE_CAPACITY; larger layouts spill.
    class OffsetList {
    public:
        static constexpr size_t INLINE_CAPACITY = 16;

        OffsetList() = default;
        OffsetList(const OffsetList& other) { Append(other.begin(), other.end()); }
        OffsetList(OffsetList&& other) noexcept { Take(other); }
        OffsetList& operator=(const OffsetList& other)
        {
            if (this != &other) {
                count = 0;
                Append(other.begin(), other.end());
            }
            return *this;
        }
        OffsetList& operator=(OffsetList&& other) noexcept
        {
            if (this != &other) {
                Release();
                Take(other);
            }
            return *this;
        }
        ~OffsetList() { Release(); }

        bool empty() const { return count == 0; }
        size_t size() const { return count; }
        size_t* begin() { return data(); }
        size_t* end() { return data() + count; }
        const size_t* begin() const { return data(); }
        const size_t* end() const { return data() + count; }
        size_t operator[](size_t index) const { return data()[index]; }

        void push_back(size_t offset)
        {
            if (count == capacity) { Grow(capacity * 2); }
            data()[count++] = offset;
        }
        size_t* erase(size_t* first, size_t* last)
        {
            count = static_cast<size_t>(std::copy(last, end(), first) - data());
            return first;
        }

    private:
        size_t* data() { return spill != nullptr ? spill : inlineSlots; }
        const size_t* data() const { return spill != nullptr ? spill : inlineSlots; }
        void Append(const size_t* first, const size_t* last)
        {
            const size_t needed = count + static_cast<size_t>(last - first);
            if (needed > capacity) { Grow(std::max(needed, capacity * 2)); }
            std::copy(first, last, data() + count);
            count = needed;
        }
        void Grow(size_t newCapacity)
        {
            size_t* const grown = new size_t[newCapacity];
            std::copy(begin(), end(), grown);
            delete[] spill;
            spill = grown;
            capacity = newCapacity;
        }
        void Take(OffsetList& other)
        {
            if (other.spill != nullptr) {
                spill = other.spill;
                capacity = other.capacity;
                count = other.count;
                other.spill = nullptr;
                other.capacity = INLINE_CAPACITY;
                other.count = 0;
            } else {
                std::copy(other.begin(), other.end(), inlineSlots);
                spill = nullptr;
                capacity = INLINE_CAPACITY;
                count = other.count;
            }
        }
        void Release()
        {
            delete[] spill;
            spill = nullptr;
            capacity = INLINE_CAPACITY;
            count = 0;
        }

        size_t* spill = nullptr;
        size_t count = 0;
        size_t capacity = INLINE_CAPACITY;
        size_t inlineSlots[INLINE_CAPACITY];
    };

    enum class Kind { Heap, Native, Uncolored };
    MAddress address;
    size_t size;
    Kind kind;
    OffsetList offsets;
    ValuePayload(MAddress address, size_t size);
    ValuePayload(MAddress address, size_t size, Kind kind);
    ValuePayload(MAddress address, size_t size, GCTib layout, Kind kind);
    ValuePayload(MAddress address, size_t size, BaseObject* layout, MAddress layoutStart);
    ValuePayload(MAddress address, size_t size, std::vector<size_t> offsets, Kind kind);
};
} // namespace MapleRuntime
#endif
