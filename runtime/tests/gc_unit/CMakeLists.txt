# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# GC unit tests — default OFF via MRT_GC_UNIT_TESTS (see runtime/config.cmake).
# U3–U7 link product libcangjie-runtime (live heap fixture); U1/U2 stay header-only.

set(GC_UNIT_SOURCES
    gc_unit_main.cpp
    gc_worker_fixture.cpp
    gc_cycle_sequence_fixture.cpp
    test_colour_address.cpp
    test_zBitField.cpp
    test_zBitMap.cpp
    test_zRememberedSet.cpp
    test_zList.cpp
    test_zArray.cpp
    test_zIndexDistributor.cpp
    test_zValue.cpp
    test_zUtils.cpp
    test_align.cpp
    test_used_generation.cpp
    test_zstat.cpp
    test_zstat_heap.cpp
    test_zserviceability.cpp
    test_trustp1_phase1.cpp

    test_defect_regressions.cpp
    test_zLiveMap.cpp
    test_region_age.cpp
    test_unwind_regressions.cpp
    test_stackmap_base_capture.cpp
    test_gctibzero.cpp
    test_field_iterator.cpp
    test_pinroot.cpp
    test_followedge.cpp
    test_colour_is_checks.cpp
    test_remap_young_roots.cpp
    test_zForwarding.cpp
    test_z_forwarding_table.cpp
    test_young_conc.cpp
    test_alloc_buffer_handoff.cpp
    test_allocation_stall_queue.cpp
    test_export_root_release.cpp
    test_tlab_usage.cpp
    test_raw_null_allocation.cpp
    test_object_allocator_paths.cpp
    test_shared_small_page.cpp
    test_young_weak.cpp
    test_native_root_current.cpp
    test_concurrency_root_color.cpp
    test_relocation_set_selector.cpp
    test_store_barrier_buffer.cpp
    test_old_to_young_1102.cpp
    test_barrier_old_atomic.cpp
    test_zPageAge.cpp
    test_zPage.cpp
    test_value_root_identity.cpp
    test_zVirtualMemory.cpp
    test_zVirtualMemoryManager.cpp
    test_mapped_cache.cpp
    test_page_retirement.cpp
    test_stay_young.cpp
    test_gc_trigger.cpp
    test_gc_director.cpp
    test_gc_request_sync.cpp
    test_p05_heuristics.cpp

    test_string_dedup.cpp
    test_concurrent_gc_breakpoints.cpp
    test_runtime_config.cpp
    test_uncommitter.cpp
    test_gc_thread_pool.cpp
    test_zWorkers.cpp
    test_zGeneration.cpp
    test_generation_mark_free.cpp

    test_exempt_unlock.cpp
    test_isfromreg.cpp
    test_current_object_ref.cpp
    test_i2_readref.cpp
    test_loadfc.cpp
    test_upstream_demangle.cpp
    test_fnlz_roots.cpp
    test_reference_processor.cpp
    test_mark_stack_entry.cpp
    test_mark_stripe.cpp
    test_mark_port_203_storage.cpp
    test_mark_port_203_entries.cpp
    test_mark_discovery.cpp
    test_mark_port_203_engine.cpp
    test_partial_array.cpp
    test_segmented_array_init.cpp
    test_package_init.cpp
    test_verify_roots.cpp
    test_p10_roots_iterator.cpp
    test_weak_roots.cpp
    test_sync_native_wait.cpp
    test_verify_fail_close.cpp
    test_verify_phase.cpp
    test_verify_marking_stacks.cpp
    test_colour_census.cpp
    test_payload_clamp.cpp
    test_write_generic.cpp
    test_arraycopy.cpp
    test_cycle_ref_saferegion.cpp
    test_thread_smr_reclaim.cpp
)

add_executable(cj_gc_unit ${GC_UNIT_SOURCES})
target_include_directories(cj_gc_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/Loader/BinaryFile
    ${CMAKE_SOURCE_DIR}/src/Heap
    ${CMAKE_SOURCE_DIR}/src/Heap/z/os/linux
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/src/Heap
    ${CMAKE_BINARY_DIR}/src/Heap/Allocator
)
target_compile_features(cj_gc_unit PRIVATE cxx_std_17)
# The product target disables C++ exceptions globally, while this deliberately
# tiny test runner reports assertion failures with throw/catch.
target_compile_options(cj_gc_unit PRIVATE -fno-rtti -fexceptions -std=gnu++17)
# Keep inline product helpers out of the test ELF's dynamic definition table.
# Product-path tests below resolve their entry points explicitly with dlsym;
# this prevents unrelated fixture TUs from exporting weak inline copies.
target_compile_options(cj_gc_unit PRIVATE -fvisibility-inlines-hidden)
target_link_options(cj_gc_unit PRIVATE -Wl,--exclude-libs,ALL)
# ZStat is always present in the product SO; use it instead of a second implementation.
target_compile_definitions(cj_gc_unit PRIVATE
    MRT_TESTABLE_INTERNALS=1
    MRT_GC_UNIT_TESTS=1)
# Prefer in-tree runtime library when building as part of the product tree.
if (TARGET cangjie-runtime)
    target_link_libraries(cj_gc_unit PRIVATE cangjie-runtime)
elseif (DEFINED GCV2_RUNTIME_LIB_DIR)
    target_link_directories(cj_gc_unit PRIVATE ${GCV2_RUNTIME_LIB_DIR})
    target_link_libraries(cj_gc_unit PRIVATE cangjie-runtime boundscheck)
    set_target_properties(cj_gc_unit PROPERTIES
        BUILD_RPATH "${GCV2_RUNTIME_LIB_DIR}"
        INSTALL_RPATH "${GCV2_RUNTIME_LIB_DIR}")
endif()

add_library(cj_package_init_fixture SHARED package_init_image.cpp)
target_include_directories(cj_package_init_fixture PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/Heap ${CMAKE_SOURCE_DIR}/include)
add_library(cj_package_init_unrelated SHARED package_init_image.cpp)
set_target_properties(cj_package_init_unrelated PROPERTIES OUTPUT_NAME cj_package_init_unrelated)
target_include_directories(cj_package_init_unrelated PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/Heap ${CMAKE_SOURCE_DIR}/include)
add_dependencies(cj_gc_unit cj_package_init_fixture cj_package_init_unrelated)

enable_testing()
add_test(NAME cj_gc_unit COMMAND cj_gc_unit)

# Isolated product-path arm for allocation stalls. The aggregate target can be
# blocked by an unrelated suite link failure; this target still binds every
# stall consumer from the same MRT_GC_UNIT_TESTS product SO.
add_executable(cj_gc_allocation_stall_unit
    gc_unit_main.cpp
    gc_cycle_sequence_fixture.cpp
)
target_include_directories(cj_gc_allocation_stall_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/Heap
    ${CMAKE_SOURCE_DIR}/src/Heap/z/os/linux
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(cj_gc_allocation_stall_unit PRIVATE cxx_std_17)
target_compile_options(cj_gc_allocation_stall_unit PRIVATE -fno-rtti -fexceptions -std=gnu++17)
target_compile_definitions(cj_gc_allocation_stall_unit PRIVATE MRT_GC_UNIT_TESTS=1 MRT_TESTABLE_INTERNALS=1)
if (TARGET cangjie-runtime)
    target_link_libraries(cj_gc_allocation_stall_unit PRIVATE cangjie-runtime)
elseif (DEFINED GCV2_RUNTIME_LIB_DIR)
    target_link_directories(cj_gc_allocation_stall_unit PRIVATE ${GCV2_RUNTIME_LIB_DIR})
    target_link_libraries(cj_gc_allocation_stall_unit PRIVATE cangjie-runtime boundscheck)
    set_target_properties(cj_gc_allocation_stall_unit PROPERTIES
        BUILD_RPATH "${GCV2_RUNTIME_LIB_DIR}"
        INSTALL_RPATH "${GCV2_RUNTIME_LIB_DIR}")
endif()
add_test(NAME cj_gc_allocation_stall_unit COMMAND cj_gc_allocation_stall_unit)

# Isolated product-path arm for the cycle-root/saferegion lock order.  The test
# calls both ZCrossVM::ResolveCycleRef and its real root consumer from the
# MRT_GC_UNIT_TESTS product SO; it does not compile either implementation into
# the test executable.
add_executable(cj_gc_cycle_saferegion_unit
    gc_unit_main.cpp
    gc_cycle_sequence_fixture.cpp
    test_cycle_ref_saferegion.cpp
)
target_include_directories(cj_gc_cycle_saferegion_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/Heap
    ${CMAKE_SOURCE_DIR}/src/Heap/z/os/linux
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(cj_gc_cycle_saferegion_unit PRIVATE cxx_std_17)
target_compile_options(cj_gc_cycle_saferegion_unit PRIVATE -fno-rtti -fexceptions -std=gnu++17)
target_compile_definitions(cj_gc_cycle_saferegion_unit PRIVATE MRT_GC_UNIT_TESTS=1 MRT_TESTABLE_INTERNALS=1)
if (TARGET cangjie-runtime)
    target_link_libraries(cj_gc_cycle_saferegion_unit PRIVATE cangjie-runtime)
elseif (DEFINED GCV2_RUNTIME_LIB_DIR)
    target_link_directories(cj_gc_cycle_saferegion_unit PRIVATE ${GCV2_RUNTIME_LIB_DIR})
    target_link_libraries(cj_gc_cycle_saferegion_unit PRIVATE cangjie-runtime boundscheck)
    set_target_properties(cj_gc_cycle_saferegion_unit PROPERTIES
        BUILD_RPATH "${GCV2_RUNTIME_LIB_DIR}"
        INSTALL_RPATH "${GCV2_RUNTIME_LIB_DIR}")
endif()
add_test(NAME cj_gc_cycle_saferegion_unit COMMAND cj_gc_cycle_saferegion_unit)

# Product-link proof for ForwardingTable::ClearEntries.  Keep this in a fresh
# process because ForwardingTable::Initialize is deliberately one-shot and the
# main suite already owns that process-global table.
add_executable(cj_gc_clear_entries_unit
    gc_unit_main.cpp
    gc_cycle_sequence_fixture.cpp
    clear_entries_product_unit.cpp
)
target_include_directories(cj_gc_clear_entries_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/Heap
    ${CMAKE_SOURCE_DIR}/src/Heap/z/os/linux
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(cj_gc_clear_entries_unit PRIVATE cxx_std_14)
target_compile_options(cj_gc_clear_entries_unit PRIVATE -fno-rtti -fexceptions -std=gnu++14)
target_compile_definitions(cj_gc_clear_entries_unit PRIVATE MRT_TESTABLE_INTERNALS=1)
if (MRT_TESTABLE_INTERNALS)
    # This macro describes the linked product shape.  MRT_TESTABLE_INTERNALS on
    # the test target itself only exposes fixture access; the product SO owns
    # the remap receipt endpoints.
    target_compile_definitions(cj_gc_clear_entries_unit PRIVATE
        MRT_PRODUCT_TESTABLE_INTERNALS=1)
endif()
if (TARGET cangjie-runtime)
    target_link_libraries(cj_gc_clear_entries_unit PRIVATE cangjie-runtime)
elseif (DEFINED GCV2_RUNTIME_LIB_DIR)
    target_link_directories(cj_gc_clear_entries_unit PRIVATE ${GCV2_RUNTIME_LIB_DIR})
    target_link_libraries(cj_gc_clear_entries_unit PRIVATE cangjie-runtime boundscheck)
    set_target_properties(cj_gc_clear_entries_unit PROPERTIES
        BUILD_RPATH "${GCV2_RUNTIME_LIB_DIR}"
        INSTALL_RPATH "${GCV2_RUNTIME_LIB_DIR}")
endif()
add_test(NAME cj_gc_clear_entries_unit COMMAND cj_gc_clear_entries_unit)

add_executable(cj_gc_uncommit_unit
    gc_unit_main.cpp
    gc_cycle_sequence_fixture.cpp
    test_string_dedup.cpp
    test_concurrent_gc_breakpoints.cpp
    test_runtime_config.cpp
    test_uncommitter.cpp
)
target_include_directories(cj_gc_uncommit_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/Heap
    ${CMAKE_SOURCE_DIR}/src/Heap/z/os/linux
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(cj_gc_uncommit_unit PRIVATE cxx_std_17)
target_compile_options(cj_gc_uncommit_unit PRIVATE -fno-rtti -fexceptions -std=gnu++17)
target_compile_definitions(cj_gc_uncommit_unit PRIVATE
    MRT_TESTABLE_INTERNALS=1
    MRT_GC_UNIT_TESTS=1)
if (TARGET cangjie-runtime)
    target_link_libraries(cj_gc_uncommit_unit PRIVATE cangjie-runtime)
elseif (DEFINED GCV2_RUNTIME_LIB_DIR)
    target_link_directories(cj_gc_uncommit_unit PRIVATE ${GCV2_RUNTIME_LIB_DIR})
    target_link_libraries(cj_gc_uncommit_unit PRIVATE cangjie-runtime boundscheck)
    set_target_properties(cj_gc_uncommit_unit PROPERTIES
        BUILD_RPATH "${GCV2_RUNTIME_LIB_DIR}"
        INSTALL_RPATH "${GCV2_RUNTIME_LIB_DIR}")
endif()
add_test(NAME cj_gc_uncommit_unit COMMAND cj_gc_uncommit_unit)

# Standalone other-vm harness regression runner; owns main and needs no runtime SO.
add_executable(cj_gc_other_vm_exit_unit test_other_vm_exit.cpp)
target_compile_features(cj_gc_other_vm_exit_unit PRIVATE cxx_std_17)
target_compile_options(cj_gc_other_vm_exit_unit PRIVATE -fexceptions)
# Do not inherit the runtime tracing archive from the parent directory.
set_property(TARGET cj_gc_other_vm_exit_unit PROPERTY LINK_LIBRARIES pthread)
add_test(NAME cj_gc_other_vm_exit_unit COMMAND cj_gc_other_vm_exit_unit)
