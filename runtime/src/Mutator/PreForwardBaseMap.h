// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_PREFORWARD_BASE_MAP_H
#define MRT_PREFORWARD_BASE_MAP_H

#include <map>

namespace MapleRuntime {
class BaseObject;

// StackMap's tagged visitor is independent of the ordinary-root closure.
// Join its result to the map owned by the synchronous preforward scan on this
// executing thread. Nested scans must restore the enclosing scan's authority.
// Transitional equivalent of ProcessDerivedOop's shared closure
// (oopMap.inline.hpp:57-123); replace with explicit closure propagation when
// the root visitor interfaces are unified.
class PreForwardBaseMapScope final {
public:
    using Map = std::map<BaseObject*, BaseObject*>;

    explicit PreForwardBaseMapScope(Map& map) : previous(Active()) { Active() = &map; }
    ~PreForwardBaseMapScope() { Active() = previous; }

    static Map* Current() { return Active(); }

    PreForwardBaseMapScope(const PreForwardBaseMapScope&) = delete;
    PreForwardBaseMapScope& operator=(const PreForwardBaseMapScope&) = delete;

private:
    static Map*& Active()
    {
        static thread_local Map* active = nullptr;
        return active;
    }
    Map* const previous;
};
} // namespace MapleRuntime
#endif
