# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# GC unit tests — default OFF via MRT_GC_UNIT_TESTS (see runtime/config.cmake).
# U3–U7 link product libcangjie-runtime (live heap fixture); U1/U2 stay header-only.

set(GC_UNIT_SOURCES
    gc_unit_main.cpp
    gc_unit_stubs.cpp
    ${CMAKE_SOURCE_DIR}/src/Base/ZStat.cpp
    test_colour_address.cpp
    test_z_bit_field.cpp
    test_z_list.cpp
    test_zstat.cpp
    test_trustp1_phase1.cpp
    test_route_info.cpp
    test_live_map.cpp
    test_object_gate.cpp
    test_remset.cpp
    test_defect_regressions.cpp
    test_region_bitmap.cpp
    test_region_age.cpp
    test_unwind_regressions.cpp
    test_gctibzero.cpp
    test_pinroot.cpp
    test_followedge.cpp
    test_z_forwarding_life.cpp
    test_colour_is_checks.cpp
    test_remap_young_roots.cpp
    test_forwarding_entries.cpp
    test_forwarding_no_geometry.cpp
    test_z_forwarding_table.cpp
    test_young_conc.cpp
    test_relocation_set_selector.cpp
    test_store_barrier_buffer.cpp
    test_page_age.cpp
    test_range_registry.cpp
    test_stay_young.cpp
    test_gc_trigger.cpp
    test_gc_request_sync.cpp
    test_mutator_relocate.cpp
    test_relocation_request_queue.cpp
    test_gc_thread_pool.cpp
    test_expire_kept.cpp
    test_receipt_life.cpp
    test_lifeclock.cpp
    test_exempt_unlock.cpp
    test_heal_coverage.cpp
    test_diag_gate.cpp
    test_interior_edge_class.cpp
    test_isfromreg.cpp
    test_current_object_ref.cpp
    test_fillerobj.cpp
    test_i2_readref.cpp
    test_m0_exit.cpp
    test_m0_correlation.cpp
    test_fwdreturn.cpp
    test_ghost_region_lookup.cpp
    test_fnlz_roots.cpp
    test_mark_stack_entry.cpp
    test_mark_stripe.cpp
    test_partial_array.cpp
    test_segmented_array_init.cpp
    test_verify_roots.cpp
    test_mem_map.cpp
)

add_executable(cj_gc_unit ${GC_UNIT_SOURCES})
target_include_directories(cj_gc_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/Heap
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(cj_gc_unit PRIVATE cxx_std_17)
# The product target disables C++ exceptions globally, while this deliberately
# tiny test runner reports assertion failures with throw/catch.
target_compile_options(cj_gc_unit PRIVATE -fno-rtti -fexceptions -std=gnu++17)
# The suite tests the real ZStat implementation regardless of whether the product .so was
# built with MRT_ZSTAT: it compiles Base/ZStat.cpp itself with the compile-time gate open.
target_compile_definitions(cj_gc_unit PRIVATE
    MRT_ZSTAT_COMPILED=1
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

enable_testing()
add_test(NAME cj_gc_unit COMMAND cj_gc_unit)
if (MRT_M0_CORRELATION_EXPERIMENT)
    set_tests_properties(cj_gc_unit PROPERTIES ENVIRONMENT "MRT_GCV2_DIAG=m0corr")
endif()

# Product-link proof for ForwardingTable::ClearEntries.  Keep this in a fresh
# process because ForwardingTable::Initialize is deliberately one-shot and the
# main suite already owns that process-global table.
add_executable(cj_gc_clear_entries_unit
    gc_unit_main.cpp
    clear_entries_product_unit.cpp
)
target_include_directories(cj_gc_clear_entries_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/Heap
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(cj_gc_clear_entries_unit PRIVATE cxx_std_14)
target_compile_options(cj_gc_clear_entries_unit PRIVATE -fno-rtti -fexceptions -std=gnu++14)
target_compile_definitions(cj_gc_clear_entries_unit PRIVATE MRT_TESTABLE_INTERNALS=1)
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
