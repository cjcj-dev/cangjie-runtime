# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${BOUNDSCHECK_INCLUDE})
if (WINDOWS_FLAG MATCHES 1)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/os/Windows)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/Mutator)
endif()

if (BUILD_RUNTIME)
    add_subdirectory(Base)
    add_subdirectory(Common)
    add_subdirectory(Concurrency)
    add_subdirectory(Demangler)
    add_subdirectory(Loader)
    add_subdirectory(Inspector)
    add_subdirectory(Exception)
    add_subdirectory(Heap)
    add_subdirectory(Mutator)
    add_subdirectory(ObjectModel)
    add_subdirectory(UnwindStack)
    add_subdirectory(Sync)
    add_subdirectory(CpuProfiler)
    add_subdirectory(Interpreter)
    if (WINDOWS_FLAG MATCHES 0)
        add_subdirectory(Signal)
    endif()
    if(SANITIZER_SUPPORT_FLAG OR GWPASAN_SUPPORT_FLAG)
        add_subdirectory(Sanitizer)
    endif()

    # set(CMAKE_C_FLAGS "-O2 -fPIC -std=c99")
    # set(CMAKE_ASM_FLAGS "-O2 -fPIC -std=gnu++14")

    if ((CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64") AND (MACOS_FLAG MATCHES 1))
    set(SRC_LIST
        "CompilerCalls.cpp"
        "ExceptionManager.cpp"
        "StackManager.cpp"
        "HeapManager.cpp"
        "LoaderManager.cpp"
        "SignalManager.cpp"
        "TypeInfoManager.cpp"
        "LogManager.cpp"
        "CangjieRuntime.cpp"
        "CjScheduler.cpp"
        "CjTimer.cpp"
        "CangjieRuntimeApi.cpp"
        "arch/x86_64_macos/N2CStub.S"
        "arch/x86_64_macos/C2NStub.S"
        "arch/x86_64_macos/CalleeSavedStub.S"
        "arch/x86_64_macos/StackGrowStub.S"
        "arch/x86_64_macos/HandleSafepointStub.S"
        "arch/x86_64_macos/ExclusiveScope.S"
        "os/Linux/Loader.cpp"
        "os/Linux/Path.cpp"
    )
elseif ((CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" AND IOS_FLAG MATCHES 1) OR IOS_SIMULATOR_FLAG MATCHES 2)
    set(SRC_LIST
        "CompilerCalls.cpp"
        "ExceptionManager.cpp"
        "StackManager.cpp"
        "HeapManager.cpp"
        "LoaderManager.cpp"
        "SignalManager.cpp"
        "TypeInfoManager.cpp"
        "LogManager.cpp"
        "CangjieRuntime.cpp"
        "CjScheduler.cpp"
        "CjTimer.cpp"
        "CangjieRuntimeApi.cpp"
        "arch/x86_64_ios/N2CStub.S"
        "arch/x86_64_ios/C2NStub.S"
        "arch/x86_64_ios/CalleeSavedStub.S"
        "arch/x86_64_ios/StackGrowStub.S"
        "arch/x86_64_ios/I2NStub.S"
        "arch/x86_64_ios/HandleSafepointStub.S"
        "arch/x86_64_ios/ExclusiveScope.S"
        "Os/Linux/Loader.cpp"
        "Os/Linux/Path.cpp"
    )
elseif ((CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64") AND (WINDOWS_FLAG MATCHES 0) AND (IOS_FLAG MATCHES 0) AND (IOS_SIMULATOR_FLAG MATCHES 0))
    set(SRC_LIST
        "CompilerCalls.cpp"
        "ExceptionManager.cpp"
        "StackManager.cpp"
        "HeapManager.cpp"
        "LoaderManager.cpp"
        "SignalManager.cpp"
        "TypeInfoManager.cpp"
        "LogManager.cpp"
        "CangjieRuntime.cpp"
        "CjScheduler.cpp"
        "CjTimer.cpp"
        "CangjieRuntimeApi.cpp"
        "arch/x86_64_linux/N2CStub.S"
        "arch/x86_64_linux/C2NStub.S"
        "arch/x86_64_linux/ExclusiveScope.S"
        "arch/x86_64_linux/CalleeSavedStub.S"
        "arch/x86_64_linux/StackGrowStub.S"
        "arch/x86_64_linux/I2NStub.S"
        "arch/x86_64_linux/HandleSafepointStub.S"
        "os/Linux/Loader.cpp"
        "os/Linux/Path.cpp"
    )
elseif ((CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64") AND (MACOS_FLAG MATCHES 1))
    set(SRC_LIST
        "CompilerCalls.cpp"
        "ExceptionManager.cpp"
        "StackManager.cpp"
        "HeapManager.cpp"
        "LoaderManager.cpp"
        "SignalManager.cpp"
        "TypeInfoManager.cpp"
        "LogManager.cpp"
        "CangjieRuntime.cpp"
        "CjScheduler.cpp"
        "CjTimer.cpp"
        "CangjieRuntimeApi.cpp"
        "arch/aarch64_macos/N2CStub.S"
        "arch/aarch64_macos/C2NStub.S"
        "arch/aarch64_macos/CalleeSavedStub.S"
        "arch/aarch64_macos/StackGrowStub.S"
        "arch/aarch64_macos/HandleSafepointStub.S"
        "arch/aarch64_macos/ExclusiveScope.S"
        "os/Linux/Loader.cpp"
        "os/Linux/Path.cpp"
    )
elseif ((IOS_FLAG MATCHES 1 OR IOS_SIMULATOR_FLAG MATCHES 1) AND (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64"))
    set(SRC_LIST
        "CompilerCalls.cpp"
        "ExceptionManager.cpp"
        "StackManager.cpp"
        "HeapManager.cpp"
        "LoaderManager.cpp"
        "SignalManager.cpp"
        "TypeInfoManager.cpp"
        "LogManager.cpp"
        "CangjieRuntime.cpp"
        "CjScheduler.cpp"
        "CjTimer.cpp"
        "CangjieRuntimeApi.cpp"
        "arch/aarch64_ios/N2CStub.S"
        "arch/aarch64_ios/C2NStub.S"
        "arch/aarch64_ios/CalleeSavedStub.S"
        "arch/aarch64_ios/StackGrowStub.S"
        "arch/aarch64_ios/I2NStub.S"
        "arch/aarch64_ios/HandleSafepointStub.S"
        "arch/aarch64_ios/ExclusiveScope.S"
        "Os/Linux/Loader.cpp"
        "Os/Linux/Path.cpp"
    )
elseif ((CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64") AND (WINDOWS_FLAG MATCHES 0))
    set(SRC_LIST
        "CompilerCalls.cpp"
        "ExceptionManager.cpp"
        "StackManager.cpp"
        "HeapManager.cpp"
        "LoaderManager.cpp"
        "SignalManager.cpp"
        "TypeInfoManager.cpp"
        "LogManager.cpp"
        "CangjieRuntime.cpp"
        "CjScheduler.cpp"
        "CjTimer.cpp"
        "CangjieRuntimeApi.cpp"
        "arch/aarch64_linux/N2CStub.S"
        "arch/aarch64_linux/C2NStub.S"
        "arch/aarch64_linux/I2NStub.S"
        "arch/aarch64_linux/ExclusiveScope.S"
        "arch/aarch64_linux/CalleeSavedStub.S"
        "arch/aarch64_linux/StackGrowStub.S"
        "arch/aarch64_linux/HandleSafepointStub.S"
        "os/Linux/Loader.cpp"
        "os/Linux/Path.cpp"
    )
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(SRC_LIST
        "CompilerCalls.cpp"
        "ExceptionManager.cpp"
        "StackManager.cpp"
        "HeapManager.cpp"
        "LoaderManager.cpp"
        "SignalManager.cpp"
        "TypeInfoManager.cpp"
        "LogManager.cpp"
        "CangjieRuntime.cpp"
        "CjScheduler.cpp"
        "CjTimer.cpp"
        "CangjieRuntimeApi.cpp"
        "arch/arm_linux/N2CStub.S"
        "arch/arm_linux/C2NStub.S"
        "arch/arm_linux/CalleeSavedStub.S"
        "arch/arm_linux/StackGrowStub.S"
        "arch/arm_linux/HandleSafepointStub.S"
        "arch/arm_linux/ExclusiveScope.S"
        "os/Linux/Loader.cpp"
        "os/Linux/Path.cpp"
    )
elseif (WINDOWS_FLAG MATCHES 1)
    set(SRC_LIST
        "CompilerCalls.cpp"
        "ExceptionManager.cpp"
        "StackManager.cpp"
        "HeapManager.cpp"
        "LoaderManager.cpp"
        "LogManager.cpp"
        "TypeInfoManager.cpp"
        "CangjieRuntime.cpp"
        "CjScheduler.cpp"
        "CjTimer.cpp"
        "CangjieRuntimeApi.cpp"
        "arch/x86_64_windows/N2CStub.S"
        "arch/x86_64_windows/C2NStub.S"
        "arch/x86_64_windows/CalleeSavedStub.S"
        "arch/x86_64_windows/GetContext.S"
        "arch/x86_64_windows/HandleSafepointStub.S"
        "arch/x86_64_windows/ThrowStackOverflowStub.S"
        "arch/x86_64_windows/StackGrowStub.S"
        "arch/x86_64_windows/ExclusiveScope.S"
        "os/Windows/Loader.cpp"
        "os/Windows/Path.cpp"
        "os/Windows/UnwindWin.cpp"
        "os/Windows/WinModuleManager.cpp"
    )
endif()
    add_library(runtime STATIC ${SRC_LIST})
    add_library(cangjie-runtime SHARED ${SRC_LIST})
    add_custom_target(check_heal_slot_writes ALL
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tests/check_heal_slot_writes.py"
        COMMENT "Checking that HeapSlot compare-exchange writes stay inside HealSlot"
        VERBATIM)
    add_dependencies(runtime check_heal_slot_writes)
    add_dependencies(cangjie-runtime check_heal_slot_writes)
    add_custom_target(check_diag_not_hollow ALL
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tests/check_diag_not_hollow.py"
        COMMENT "Checking that a diagnostic documenting a gate is not an all-no-op implementation"
        VERBATIM)
    add_dependencies(runtime check_diag_not_hollow)
    add_dependencies(cangjie-runtime check_diag_not_hollow)
    # The GC unit suite is the only deterministic detector this runtime has: it names a defect in
    # tens of seconds where the workload route needs a crash, a rate over N>=10 and a forensic read.
    # It shipped behind MRT_GC_UNIT_TESTS:BOOL=OFF with a fail-closed gate script that nothing ever
    # called, so it ran zero times.  POST_BUILD rather than add_dependencies because it links the
    # freshly built .so; the script self-skips via a stamp when neither the .so nor the suite moved,
    # and known_failures.txt keeps today's two standing failures from blocking while still blocking
    # anything new -- and blocking a listed test that starts passing, so the list cannot rot.
    if (NOT CMAKE_CROSSCOMPILING AND NOT WINDOWS_FLAG MATCHES 1)
        add_custom_command(TARGET cangjie-runtime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E env
                "GCV2_RUNTIME_LIB_DIR=$<TARGET_FILE_DIR:cangjie-runtime>"
                bash "${CMAKE_CURRENT_SOURCE_DIR}/../tests/gc_unit/gate_gc_unit.sh"
            COMMENT "Running the deterministic GC unit suite against the runtime just built"
            VERBATIM)
    endif()
    if (WINDOWS_FLAG MATCHES 1)
        add_custom_command(TARGET cangjie-runtime POST_BUILD
            COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../build/generate_windows_exports.py" check
                "${CMAKE_BINARY_DIR}/windows_x86_64_exports.raw.def"
                "${CMAKE_CURRENT_SOURCE_DIR}/windows_x86_64_exports.def"
            COMMENT "Checking the Windows runtime export surface"
            VERBATIM)
    endif()
    if (((IOS_FLAG MATCHES 1 OR IOS_SIMULATOR_FLAG MATCHES 1) AND (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")) OR
        ((CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" AND IOS_FLAG MATCHES 1) OR IOS_SIMULATOR_FLAG MATCHES 2))
	target_compile_options(cangjie-runtime PRIVATE -Wno-elaborated-enum-base)
	target_compile_options(cangjie-runtime PRIVATE -Wno-availability)
	target_compile_options(runtime PRIVATE -Wno-elaborated-enum-base)
	target_compile_options(runtime PRIVATE -Wno-availability)
	find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    if (COREFOUNDATION_FRAMWORK)
	    target_link_libraries(cangjie-runtime ${COREFOUNDATION_FRAMEWORK}/CoreFoundation.tbd)
	    target_link_libraries(runtime ${COREFOUNDATION_FRAMEWORK}/CoreFoundation.tbd)
    endif()
endif()

elseif (BUILD_DEMANGLE)
    add_subdirectory(Demangler)
endif() # BUILD_RUNTIME
