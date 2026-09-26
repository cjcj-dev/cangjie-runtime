// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_ROSALLOC_DEQUE_H
#define MRT_ROSALLOC_DEQUE_H

#include <cstddef>
#include <cstdint>

#include <new>
#include <sys/mman.h>

#include "Base/Log.h"
#include "Base/Panic.h"
#include "Base/SysCall.h"
#include "Heap/Allocator/AllocUtil.h"

#define DEBUG_DEQUE false
#if DEBUG_DEQUE
#define DEQUE_ASSERT(cond, msg) MRT_ASSERT(cond, msg)
#else
#define DEQUE_ASSERT(cond, msg) (void(0))
#endif

namespace MapleRuntime {
// Committed anonymous mapping backing the deques below. ZGC has no deque of
// this kind; this is old-directory residue that leaves with CartesianTree.
class DequeMapping {
public:
    static DequeMapping* MapMemory(size_t size, const char* tag)
    {
        void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK_DETAIL(base != MAP_FAILED, "%s: mmap of %zu bytes failed", tag, size);
#if defined(__linux__) || defined(hongmeng)
        MRT_PRCTL(base, size, tag);
#endif
        return new (std::nothrow) DequeMapping(base, size);
    }

    static void DestroyMemMap(DequeMapping*& mapping) noexcept
    {
        if (mapping != nullptr) {
            (void)munmap(mapping->base, mapping->size);
            delete mapping;
            mapping = nullptr;
        }
    }

    void* GetBaseAddr() const { return base; }
    void* GetCurrEnd() const { return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + size); }

private:
    DequeMapping(void* mappedBase, size_t mappedSize) : base(mappedBase), size(mappedSize) {}
    void* base;
    size_t size;
};

// this deque is single-use, meaning we can use it, then after a while,
// we can discard its whole content
// under this assumption, clearing this data structure is really fast
// (each clear takes O(1) time)
// also, this assumes that the underlying memory does not need to be freed
// because we can reuse it after each clear
//
// it can be used like a queue or a stack
template<class ValType>
class SingleUseDeque {
public:
    static constexpr size_t VAL_SIZE = sizeof(ValType);
    void Init(size_t mapSize)
    {
        static_assert(VAL_SIZE == sizeof(void*), "invalid val type");
        memMap = DequeMapping::MapMemory(mapSize, "maple_alloc_ros_sud");
        beginAddr = reinterpret_cast<MAddress>(memMap->GetBaseAddr());
        endAddr = reinterpret_cast<MAddress>(memMap->GetCurrEnd());
        Clear();
    }
    void Init(DequeMapping& other)
    {
        // init from another sud, that is, the two suds share the same mem map
        static_assert(VAL_SIZE == sizeof(void*), "invalid val type");
        memMap = &other;
        beginAddr = reinterpret_cast<MAddress>(memMap->GetBaseAddr());
        endAddr = reinterpret_cast<MAddress>(memMap->GetCurrEnd());
        Clear();
    }
    void Fini() noexcept { DequeMapping::DestroyMemMap(memMap); }
    DequeMapping& GetMemMap() { return *memMap; }
    bool Empty() const { return topAddr < frontAddr; }
    void Push(ValType v)
    {
        topAddr += VAL_SIZE;
        DEQUE_ASSERT(topAddr < endAddr, "not enough memory");
        *reinterpret_cast<ValType*>(topAddr) = v;
    }
    ValType Top()
    {
        DEQUE_ASSERT(topAddr >= frontAddr, "read empty queue");
        return *reinterpret_cast<ValType*>(topAddr);
    }
    void Pop()
    {
        DEQUE_ASSERT(topAddr >= frontAddr, "pop empty queue");
        topAddr -= VAL_SIZE;
    }
    ValType Front()
    {
        DEQUE_ASSERT(frontAddr <= topAddr, "front reach end");
        return *reinterpret_cast<ValType*>(frontAddr);
    }
    void PopFront()
    {
        DEQUE_ASSERT(frontAddr <= topAddr, "pop front empty queue");
        frontAddr += VAL_SIZE;
    }
    void Clear()
    {
        frontAddr = beginAddr;
        topAddr = beginAddr - VAL_SIZE;
    }

private:
    DequeMapping* memMap = nullptr;
    MAddress beginAddr = 0;
    MAddress frontAddr = 0;
    MAddress topAddr = 0;
    MAddress endAddr = 0;
};

// this deque lives on the stack, hence local
// this is better than the above deque because it avoids visiting ram
// and it also avoids using unfreeable memory
// however, its capacity is limited
template<class ValType>
class LocalDeque {
public:
    static_assert(sizeof(ValType) == sizeof(void*), "invalid val type");
    static constexpr int LOCAL_LENGTH = ALLOC_UTIL_PAGE_SIZE / sizeof(ValType);
    explicit LocalDeque(SingleUseDeque<ValType>& singleUseDeque) : sud(&singleUseDeque) {}
    ~LocalDeque() = default;
    bool Empty() const { return (top < front) || (front == LOCAL_LENGTH && sud->Empty()); }
    void Push(ValType v)
    {
        if (LIKELY(top < LOCAL_LENGTH - 1)) {
            array[++top] = v;
            return;
        } else if (top == LOCAL_LENGTH - 1) {
            ++top;
            sud->Clear();
        }
        sud->Push(v);
    }
    ValType Top()
    {
        if (LIKELY(top < LOCAL_LENGTH)) {
            DEQUE_ASSERT(top >= front, "read empty queue");
            return array[top];
        }
        return sud->Top();
    }
    void Pop()
    {
        if (LIKELY(top < LOCAL_LENGTH)) {
            DEQUE_ASSERT(top >= front, "pop empty queue");
            --top;
            return;
        }
        DEQUE_ASSERT(top == LOCAL_LENGTH, "pop error");
        sud->Pop();
        if (sud->Empty()) {
            // if local array is empty, reuse loacl array
            if (front == LOCAL_LENGTH) {
                front = 0;
                top = -1;
            } else if (front < LOCAL_LENGTH) {
                --top;
                return;
            }
        }
    }
    ValType Front()
    {
        if (LIKELY(front < LOCAL_LENGTH)) {
            DEQUE_ASSERT(front <= top, "read empty queue front");
            return array[front];
        }
        DEQUE_ASSERT(top == LOCAL_LENGTH, "queue front error");
        return sud->Front();
    }
    void PopFront()
    {
        if (LIKELY(front < LOCAL_LENGTH)) {
            DEQUE_ASSERT(front <= top, "pop front empty queue");
            ++front;
            return;
        }
        DEQUE_ASSERT(front == LOCAL_LENGTH, "pop front error");
        sud->PopFront();
    }

private:
    int front = 0;
    int top = -1;
    SingleUseDeque<ValType>* sud;
    ValType array[LOCAL_LENGTH] = { 0 };
};

// this allocator allocates for a certain-sized native data structure
// it is very lightweight but doesn't recycle pages.
template<size_t allocSize, size_t align>
class RTAllocatorT {
    struct Slot {
        Slot* next = nullptr;
    };

public:
    void Init(size_t mapSize)
    {
        static_assert(allocSize >= sizeof(Slot), "invalid alloc size");
        static_assert(align >= alignof(Slot), "invalid align");
        static_assert(allocSize % align == 0, "size not aligned");

        memMap = DequeMapping::MapMemory(mapSize, "maplert_alloc");
        currAddr = reinterpret_cast<MAddress>(memMap->GetBaseAddr());
        endAddr = reinterpret_cast<MAddress>(memMap->GetCurrEnd());
    }

    void Fini() noexcept { DequeMapping::DestroyMemMap(memMap); }

    void* Allocate()
    {
        void* result = nullptr;
        if (UNLIKELY(this->head == nullptr)) {
            DEQUE_ASSERT(this->currAddr + allocSize <= this->endAddr, "not enough memory");
            result = reinterpret_cast<void*>(this->currAddr);
            this->currAddr += allocSize;
        } else {
            result = reinterpret_cast<void*>(this->head);
            this->head = this->head->next;
        }
        // no zeroing
        return result;
    }

    void Deallocate(void* addr)
    {
        reinterpret_cast<Slot*>(addr)->next = this->head;
        this->head = reinterpret_cast<Slot*>(addr);
    }

private:
    Slot* head = nullptr;
    MAddress currAddr = 0;
    MAddress endAddr = 0;
    DequeMapping* memMap = nullptr;
};
} // namespace MapleRuntime

#endif // MRT_ROSALLOC_DEQUE_H
