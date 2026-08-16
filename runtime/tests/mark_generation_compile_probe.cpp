// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// genface compile contract:
//   control                                  -> MUST compile
//   MRT_NEGATIVE_YOUNG_AS_OLD               -> MUST fail at RequireOld
//   MRT_NEGATIVE_OLD_AS_YOUNG               -> MUST fail at RequireYoung

#include <type_traits>

#include "Heap/Collector/LiveInfo.h"

using MapleRuntime::Generation;
using MapleRuntime::MarkView;

static_assert(!std::is_convertible<MarkView<Generation::Young>, MarkView<Generation::Old>>::value,
              "young mark view must not convert to old");
static_assert(!std::is_convertible<MarkView<Generation::Old>, MarkView<Generation::Young>>::value,
              "old mark view must not convert to young");

void RequireOld(MarkView<Generation::Old>);
void RequireYoung(MarkView<Generation::Young>);

void CorrectOld(MarkView<Generation::Old> view)
{
    RequireOld(view);
}

void CorrectYoung(MarkView<Generation::Young> view)
{
    RequireYoung(view);
}

#if defined(MRT_NEGATIVE_YOUNG_AS_OLD)
void WrongYoungAsOld(MarkView<Generation::Young> view)
{
    RequireOld(view);
}
#endif

#if defined(MRT_NEGATIVE_OLD_AS_YOUNG)
void WrongOldAsYoung(MarkView<Generation::Old> view)
{
    RequireYoung(view);
}
#endif
