// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_REFERENCE_PROCESSOR_H
#define MRT_REFERENCE_PROCESSOR_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "Common/TypeDef.h"

namespace MapleRuntime {

// Cangjie currently has product objects for weak references and finalizers.
// Soft/phantom are named here so discovery cannot silently pretend that a JVM
// object layout exists when it does not.
enum class ReferenceType : uint8_t {
    SOFT = 0,
    WEAK,
    FINAL,
    PHANTOM,
    COUNT,
};

enum class ReferenceStatus : uint8_t {
    DISCOVERED,
    INACTIVE,
    UNSUPPORTED,
};

class ReferenceProcessor {
public:
    static constexpr size_t REFERENCE_TYPE_COUNT = static_cast<size_t>(ReferenceType::COUNT);
    using IsStronglyLive = std::function<bool(BaseObject*)>;
    using EnqueueFinal = std::function<void(BaseObject*)>;
    using ObserveWeakFinal = std::function<void(BaseObject*, BaseObject*)>;

    ReferenceProcessor();
    ~ReferenceProcessor();
    ReferenceProcessor(const ReferenceProcessor&) = delete;
    ReferenceProcessor& operator=(const ReferenceProcessor&) = delete;

    static constexpr bool IsSupported(ReferenceType type)
    {
        return type == ReferenceType::WEAK || type == ReferenceType::FINAL;
    }

    ReferenceStatus DiscoverReference(BaseObject* reference, ReferenceType type);
    void ProcessReferences(const IsStronglyLive& isStronglyLive);
    void EnqueueReferences(const EnqueueFinal& enqueueFinal);
#if defined(MRT_TESTABLE_INTERNALS)
    void EnqueueReferences(const EnqueueFinal& enqueueFinal, const ObserveWeakFinal& observeWeakFinal);
    static void SetBeforeWeakCleanCasForTest(std::function<void()> hook);
#endif
    static bool IsFinalizable(BaseObject* reference);
    static bool CleanWeakReference(BaseObject* reference);

    size_t Encountered(ReferenceType type) const;
    size_t Discovered(ReferenceType type) const;
    size_t Enqueued(ReferenceType type) const;
    bool Empty() const;

private:
    struct Node {
        BaseObject* reference;
        ReferenceType type;
        Node* next;
    };

    static constexpr size_t TypeIndex(ReferenceType type) { return static_cast<size_t>(type); }
    struct WeakCleanResult {
        bool cleared;
        bool casLost;
        BaseObject* terminalReferent;
    };

    static void Push(std::atomic<Node*>& head, Node* node);
    static void DeleteList(Node* list);
    static WeakCleanResult CleanWeakReferenceWithResult(BaseObject* reference);
    void EnqueueReferencesImpl(const EnqueueFinal& enqueueFinal, const ObserveWeakFinal& observeWeakFinal);

    std::atomic<Node*> discoveredList{ nullptr };
    std::atomic<Node*> pendingList{ nullptr };
    std::array<std::atomic<size_t>, REFERENCE_TYPE_COUNT> encountered{};
    std::array<std::atomic<size_t>, REFERENCE_TYPE_COUNT> discovered{};
    std::array<std::atomic<size_t>, REFERENCE_TYPE_COUNT> enqueued{};
};

} // namespace MapleRuntime
#endif // MRT_REFERENCE_PROCESSOR_H
