// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include "Heap/z/zForwarding.hpp"

namespace MapleRuntime {

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

}

namespace MapleRuntime {

}

namespace MapleRuntime {

}

namespace MapleRuntime {

}

namespace MapleRuntime {

}

namespace MapleRuntime {

}
