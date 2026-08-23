// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// typefence phase ① — CurrentObjectRef mint/escape compile-time contract.
// Shape: test_trustp1_phase1.cpp / RefField.h deleted HeapSlotAt overloads.

#include "ObjectModel/CurrentObjectRef.h"
#include "gc_unittest.hpp"

#include <cstdint>
#include <type_traits>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(TypeFence, SizeIsOneWord)
{
    GC_EXPECT_EQ(sizeof(CurrentObjectRef), sizeof(void*));
}

GC_TEST(TypeFence, NullDefault)
{
    CurrentObjectRef r;
    GC_EXPECT_TRUE(r.isNull());
    GC_EXPECT_TRUE(r.unsafeRaw() == nullptr);
}

GC_TEST(TypeFence, FromResolvedRoundTrip)
{
    BaseObject* fake = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x1000));
    CurrentObjectRef r = CurrentObjectRef::fromResolved(fake);
    GC_EXPECT_FALSE(r.isNull());
    GC_EXPECT_TRUE(r.unsafeRaw() == fake);
}

GC_TEST(TypeFence, CopyPreservesBits)
{
    BaseObject* fake = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x2000));
    CurrentObjectRef a = CurrentObjectRef::fromResolved(fake);
    CurrentObjectRef b = a;
    GC_EXPECT_TRUE(a == b);
    GC_EXPECT_TRUE(b.unsafeRaw() == fake);
}

GC_TEST(TypeFence, MaybeFromIsNotConstructible)
{
    static_assert(!std::is_constructible<CurrentObjectRef, BaseObject*>::value, "");
    static_assert(!std::is_constructible<CurrentObjectRef, const BaseObject*>::value, "");
    static_assert(!std::is_convertible<BaseObject*, CurrentObjectRef>::value, "");
    static_assert(!std::is_convertible<CurrentObjectRef, BaseObject*>::value, "");
    static_assert(!std::is_assignable<CurrentObjectRef, BaseObject*>::value, "");
    GC_EXPECT_TRUE(true);
}
