// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#ifndef GC_UNIT_ROOT_PUBLICATION_SNAPSHOT_HPP
#define GC_UNIT_ROOT_PUBLICATION_SNAPSHOT_HPP

#if defined(MRT_TESTABLE_INTERNALS)
#include <functional>
#include <set>

#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"

namespace MapleRuntime {
// Test-side, read-only copy of what a ZMark has actually published.
// zMarkStack.hpp names this struct as a friend of MarkStripeStack,
// MarkStripeStackList and MarkStripe; the product itself has no observer
// (ZGC zMarkStack.hpp:35-54 keeps the chunk single-owner while filled and
// immutable while linked on a stripe).
//
// Only valid while no worker pops: after the root task has returned and
// before TracingImpl starts, i.e. inside CopyCollector::testOldMarkStarted
// (zGeneration.cpp, DoTracing). Producers may still prepend a node, which is
// why the walk starts from an acquire load of the list head.
struct RootPublicationSnapshot {
    using Visitor = std::function<void(const MarkStackEntry&)>;

    static void VisitList(const MarkStripeStackList& list, const Visitor& visit)
    {
        for (auto* node = list.head.load(std::memory_order_acquire); node != nullptr; node = node->Next()) {
            const MarkStripeStack* stack = node->Stack();
            const MarkStackEntry* entries = stack->entries(stack);
            for (size_t i = 0; i < stack->top; ++i) {
                if (!entries[i].partial_array()) {
                    visit(entries[i]);
                }
            }
        }
    }

    static void Visit(ZMark& domain, const Visitor& visit)
    {
        for (size_t i = 0; i < domain.Stripes().Count(); ++i) {
            const MarkStripe& stripe = domain.Stripes().At(i);
            VisitList(stripe.published, visit);
            VisitList(stripe.overflowed, visit);
        }
    }

    static bool Contains(ZMark& domain, const BaseObject* object)
    {
        bool found = false;
        Visit(domain, [&](const MarkStackEntry& entry) { found = found || to_object(ZOffset::address(to_zoffset(entry.object_address()))) == object; });
        return found;
    }

    static std::set<BaseObject*> Objects(ZMark& domain)
    {
        std::set<BaseObject*> objects;
        Visit(domain, [&](const MarkStackEntry& entry) { objects.insert(to_object(ZOffset::address(to_zoffset(entry.object_address())))); });
        return objects;
    }
};
} // namespace MapleRuntime
#endif // MRT_TESTABLE_INTERNALS
#endif // GC_UNIT_ROOT_PUBLICATION_SNAPSHOT_HPP
