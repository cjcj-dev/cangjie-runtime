// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_PAGE_AGE_H
#define MRT_PAGE_AGE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

enum class PageAge : uint8_t {
    eden,
    survivor1,
    survivor2,
    survivor3,
    survivor4,
    survivor5,
    survivor6,
    survivor7,
    survivor8,
    survivor9,
    survivor10,
    survivor11,
    survivor12,
    survivor13,
    survivor14,
    old
};

constexpr uint32_t kPageAgeCount = static_cast<uint32_t>(PageAge::old) + 1;
constexpr PageAge kPageAgeLastPlusOne = static_cast<PageAge>(kPageAgeCount);
constexpr uint32_t kNumRelocationAges = kPageAgeCount - 1;

class PageAgeRange {
public:
    constexpr PageAgeRange() : firstAge(PageAge::eden), lastPlusOne(kPageAgeLastPlusOne) {}

    template<PageAge First, PageAge LastPlusOne>
    static constexpr PageAgeRange create()
    {
        return PageAgeRange(First, LastPlusOne);
    }

    constexpr PageAge first() const { return firstAge; }

    constexpr PageAge last() const
    {
        return static_cast<PageAge>(static_cast<uint32_t>(lastPlusOne) - 1);
    }

    class iterator {
    public:
        constexpr explicit iterator(PageAge a) : age(a) {}
        constexpr PageAge operator*() const { return age; }
        constexpr iterator& operator++()
        {
            age = static_cast<PageAge>(static_cast<uint32_t>(age) + 1);
            return *this;
        }
        constexpr bool operator!=(iterator other) const { return age != other.age; }

    private:
        PageAge age;
    };

    constexpr iterator begin() const { return iterator(firstAge); }
    constexpr iterator end() const { return iterator(lastPlusOne); }

private:
    constexpr PageAgeRange(PageAge first, PageAge lastExcl) : firstAge(first), lastPlusOne(lastExcl) {}

    PageAge firstAge;
    PageAge lastPlusOne;
};

constexpr PageAgeRange kPageAgeRangeEden = PageAgeRange::create<PageAge::eden, PageAge::survivor1>();
constexpr PageAgeRange kPageAgeRangeYoung = PageAgeRange::create<PageAge::eden, PageAge::old>();
constexpr PageAgeRange kPageAgeRangeSurvivor = PageAgeRange::create<PageAge::survivor1, PageAge::old>();
constexpr PageAgeRange kPageAgeRangeRelocation = PageAgeRange::create<PageAge::survivor1, kPageAgeLastPlusOne>();
constexpr PageAgeRange kPageAgeRangeOld = PageAgeRange::create<PageAge::old, kPageAgeLastPlusOne>();
constexpr PageAgeRange kPageAgeRangeAll = PageAgeRange();

constexpr uint32_t untype(PageAge age) { return static_cast<uint32_t>(age); }

constexpr PageAge to_pageage(uint32_t age)
{
    return static_cast<PageAge>(age);
}

inline PageAge operator+(PageAge age, size_t size)
{
    return to_pageage(untype(age) + static_cast<uint32_t>(size));
}

inline PageAge operator-(PageAge age, size_t size)
{
    return to_pageage(untype(age) - static_cast<uint32_t>(size));
}

} // namespace MapleRuntime

#endif
