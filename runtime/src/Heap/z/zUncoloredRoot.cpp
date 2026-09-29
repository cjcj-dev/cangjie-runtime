// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#include "Heap/z/zUncoloredRoot.hpp"

namespace MapleRuntime {
void ZUncoloredRootClosure::do_oop(RefField<>* p)
{
    do_root(ZUncoloredRoot::cast(p));
}

} // namespace MapleRuntime
