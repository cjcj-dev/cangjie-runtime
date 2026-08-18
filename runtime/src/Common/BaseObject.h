// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_BASE_OBJECT_H
#define MRT_BASE_OBJECT_H

#include "Common/StateWord.h"
#include "ObjectModel/Field.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.h"

// this is the Base class for "what's called a managed object" which can be collected.
namespace MapleRuntime {
class BaseObject {
public:
    TypeInfo* GetTypeInfo() const;

    inline bool HasRefField() const { return GetTypeInfo()->HasRefField(); }

    inline bool IsWeakRef() const { return GetTypeInfo()->IsWeakRefType(); }

    inline bool IsValidObject() const { return stateWord.IsValidStateWord(); }

    inline bool IsRawArray() const { return GetTypeInfo()->IsRawArray(); }

    inline TypeInfo* GetComponentTypeInfo() const { return GetTypeInfo()->GetComponentTypeInfo(); }

    inline GCTib GetGCTib() const { return GetTypeInfo()->GetGCTib(); }

    void ForEachRefField(const HeapSlotVisitor& visitor);

    void ForEachRefInStruct(const HeapSlotVisitor& visitor, MAddress aggStart, MAddress aggEnd);
    // size in bytes
    size_t GetSize() const;

    size_t GetSize(TypeInfo* kls) const;

    bool CompareExchangeRefField(HeapSlot<>& field, const HeapSlot<> oldRef, const HeapSlot<> newRef);

    template<bool isVolatile = false>
    HeapSlot<isVolatile>& GetRefField(U32 offset) const
    {
        auto addr = reinterpret_cast<uintptr_t>(this) + offset;
        return HeapSlotAt<isVolatile>(static_cast<MAddress>(addr));
    }

    template<typename T>
    Field<T>& GetField(U32 offset) const
    {
        auto addr = reinterpret_cast<uintptr_t>(this) + offset;
        return *reinterpret_cast<Field<T>*>(static_cast<Uptr>(addr));
    }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    void DumpObject(int logtype, bool isSimple = false) const;
#endif

    StateWord GetStateWord() const { return stateWord.GetStateWord(); }
    ObjectState GetObjectState() const { return stateWord.GetObjectState(); }

    bool IsForwarded() const { return GetObjectState().IsForwardedState(); }

    void SetClassInfo(TypeInfo* klassRef) { stateWord.SetTypeInfo(klassRef); }
    void SetStateCode(ObjectState::ObjectStateCode state) { stateWord.SetStateCode(state); }

    bool IsInTraceRegion() const;

    // when forwarding failed, we need to clear forwarding state,
    // because forwaring object can only be executed by only one thread,
    // so we don't need to worry aboout concurrent competetion

    bool TryLockObject(const StateWord curWord) { return stateWord.TryLockStateWord(curWord.GetObjectState()); }

    void UnlockObject(const ObjectState newState) { stateWord.UnlockStateWord(newState); }

    void OnFinalizerCreated();

    static intptr_t FieldOffset(const BaseObject* obj, const void* field)
    {
        return reinterpret_cast<intptr_t>(field) - reinterpret_cast<intptr_t>(obj);
    }

protected:
    // SetClassInfo turns a managed address into a valid "BaseObject"
    // can only be invoked when object initialised in order to avoid competetion.
    // caller should ensure that address is valid (not doing null check here)
    static inline BaseObject* SetClassInfo(MAddress address, TypeInfo* klass)
    {
        auto ref = from_alloc_addr(address);
        // Whole word, not just the address halves: this memory may have been a from-version, and
        // SetTypeInfo would leave its stateCode behind (StateWord::InitTypeInfoAndState).
        ref->stateWord.InitTypeInfoAndState(klass);
        return ref;
    }

private:
    // We cannot explicit construct BaseObject and destruct it
    BaseObject() = delete;
    ~BaseObject() = delete;
    void ForEachAggRefFieldInArray(const RefFieldVisitor& visitor, MAddress aggStart, MAddress aggEnd);
    void ForEachAggRefFieldInNonArray(const RefFieldVisitor& visitor, MAddress aggStart, MAddress aggEnd) const;

    // The only contract between Managed Heap and other modules
    StateWord stateWord;
};

using ObjectPtr = BaseObject*;
using ObjectVisitor = std::function<void(ObjectPtr)>;

// Stack/runtime roots are uncoloured RootSlots. ObjectRef remains as an API
// spelling only; unlike the old struct it cannot be reinterpreted as HeapSlot.
using ObjectRef = RootSlot;

using RawRefVisitor = RootSlotVisitor;
using RootVisitor = RawRefVisitor;
using StackPtrVisitor = RawRefVisitor;
} // namespace MapleRuntime

#endif // MRT_BASE_OBJECT_H
