// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_YOUNG_CLOSURE_OBSERVATION_HPP
#define MRT_YOUNG_CLOSURE_OBSERVATION_HPP
#include <unordered_set>
#include "Heap/z/zMarkStack.hpp"

namespace MapleRuntime::GcUnit {
// Observe the completed product closure before relocation can promote the page
// and change its livemap owner. This never supplies work to the collector.
class YoungClosureObservation {
public:
    YoungClosureObservation() : previous(active)
    {
        active = this;
        SetMarkClosureObserverForTest(Observe);
    }
    ~YoungClosureObservation()
    {
        active = previous;
        SetMarkClosureObserverForTest(active == nullptr ? nullptr : Observe);
    }
    bool Saw(BaseObject* object) const { return objects.count(object) != 0; }
    size_t Calls() const { return calls; }
private:
    static void Observe(const std::vector<BaseObject*>* closure)
    {
        if (active != nullptr && closure != nullptr) {
            ++active->calls;
            active->objects.insert(closure->begin(), closure->end());
        }
    }
    inline static YoungClosureObservation* active = nullptr;
    YoungClosureObservation* previous;
    std::unordered_set<BaseObject*> objects;
    size_t calls = 0;
};
}
#endif
