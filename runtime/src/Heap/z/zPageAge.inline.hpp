#pragma once
#include "Heap/z/zPageAge.hpp"
namespace MapleRuntime {
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

}
