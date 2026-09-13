// Test observation fragment; included only by zHeap.cpp after HeapImpl and its instance.
#pragma once

#ifdef MRT_TESTABLE_INTERNALS
inline size_t HeapImpl::GetStaticRootCountForTesting() { return staticRootTable.RootCountForTesting(); }
#endif

#ifdef MRT_TESTABLE_INTERNALS
size_t Heap::GetStaticRootCountForTesting()
{
    return g_heapInstance->GetStaticRootCountForTesting();
}
#endif
