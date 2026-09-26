# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "shared/gcArguments.cpp"
    "shared/jvmFlagConstraintsGC.cpp"
    "z/zHeap.cpp"
    "z/zInitialize.cpp"
    "z/zArguments.cpp"
    "z/zGlobals.cpp"
    "z/zThreadLocalData.cpp"
    "z/workerThread.cpp"
    "z/zTask.cpp"
    "z/zWorkers.cpp"
    "z/zRuntimeWorkers.cpp"
    "z/concurrentGCThread.cpp"
    "z/zThread.cpp"
    "z/zUtils.cpp"
    "z/zCPU.cpp"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
if (WINDOWS_FLAG MATCHES 1)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/z/os/windows)
elseif (MACOS_FLAG MATCHES 1 OR IOS_FLAG MATCHES 1 OR IOS_SIMULATOR_FLAG MATCHES 1 OR IOS_SIMULATOR_FLAG MATCHES 2)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/z/os/bsd)
else()
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/z/os/linux)
endif()
list(APPEND SRC_LIST
    "z/zLiveMap.cpp"

    "z/zForwardingTable.cpp"
    "z/zForwardingAllocator.cpp"
    "z/zVirtualMemoryManager.cpp"
    "z/zAddressSpaceLimit.cpp"
    "z/zNUMA.cpp"
    "z/zPhysicalMemoryManager.cpp"
    "z/zLargePages.cpp"
    "z/zErrno.cpp"
    "Allocator/RegionSpace.cpp"
    "z/zThreadLocalAllocBuffer.cpp"
    "z/zTLABUsage.cpp"
    "z/zPageAllocator.cpp"
    "z/zPage.cpp"
    "z/zPageTable.cpp"
    "z/zObjectAllocator.cpp"
    "z/zObjArrayAllocator.cpp"
    "z/zHeuristics.cpp"
    "z/zRelocationSetSelector.cpp"
    "Allocator/CartesianTree.cpp"
    "z/zMappedCache.cpp"
)

# ZGC os/<os>/gc/z/: CMake selects the pd files. Do not list posix/linux
# unconditionally, and do not keep _WIN64/__APPLE__ in the shared files.
if (WINDOWS_FLAG MATCHES 1)
    list(APPEND SRC_LIST
        "z/zVirtualMemoryManager_windows.cpp"
        "z/zPhysicalMemoryBacking_windows.cpp"
        "z/zMapper_windows.cpp"
        "z/zSyscall_windows.cpp"
        "z/zLargePages_windows.cpp"
        "z/zNUMA_windows.cpp"
    )
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/z/os/windows)
elseif (MACOS_FLAG MATCHES 1 OR IOS_FLAG MATCHES 1 OR IOS_SIMULATOR_FLAG MATCHES 1 OR IOS_SIMULATOR_FLAG MATCHES 2)
    list(APPEND SRC_LIST
        "z/zVirtualMemoryManager_posix.cpp"
        "z/zPhysicalMemoryBacking_bsd.cpp"
        "z/zLargePages_bsd.cpp"
        "z/zNUMA_bsd.cpp"
    )
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/z/os/bsd)
else()
    list(APPEND SRC_LIST
        "z/zVirtualMemoryManager_posix.cpp"
        "z/zPhysicalMemoryBacking_linux.cpp"
        "z/zMountPoint_linux.cpp"
        "z/zLargePages_linux.cpp"
        "z/zNUMA_linux.cpp"
    )
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/z/os/linux)
    add_compile_definitions(LINUX)
endif()


list(APPEND SRC_LIST
    "z/zBarrier.cpp"
    "z/zBarrierSet.cpp"
    "z/accessBarrierSupport.cpp"
    "z/zUncoloredRoot.cpp"
    "z/zRememberedSet.cpp"
    "z/zStoreBarrierBuffer.cpp"
    "z/zVerify.cpp"
)

list(APPEND SRC_LIST
    "z/zBreakpoint.cpp"
    "z/concurrentGCBreakpoints.cpp"
    "z/zAbort.cpp"
    "z/zHeapIterator.cpp"
    "z/zCollectedHeap.cpp"
    "z/zDriver.cpp"
    "z/zDirector.cpp"
    "z/zDriverPort.cpp"
    "z/zRootsIterator.cpp"
    "z/zWeakRootsProcessor.cpp"
    "z/zResurrection.cpp"
    "z/zMarkStack.cpp"
    "z/zMarkingSMR.cpp"
    "z/zMarkCache.cpp"
    "z/zReferenceProcessor.cpp"
    "shared/stringdedup/stringDedup.cpp"
    "shared/collectedHeap.cpp"
    "z/zUncommitter.cpp"
    "z/zForwarding.cpp"
)

list(APPEND SRC_LIST
    "z/zCrossVM.cpp"
    "z/zGeneration.cpp"
    "z/zMark.cpp"
    "z/zRelocate.cpp"
    "z/zJNICritical.cpp"
    "z/zRemembered.cpp"
    "z/zRelocationSet.cpp"
)

add_library(Heap STATIC ${SRC_LIST})
