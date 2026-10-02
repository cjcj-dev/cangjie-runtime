cmake_minimum_required(VERSION 3.19)
project(ManagedMetadataTests LANGUAGES CXX ASM)
get_filename_component(RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if(NOT GCV2_RUNTIME_LIB_DIR OR NOT PRODUCT_BUILD)
    message(FATAL_ERROR "Explicit product library and build directories required")
endif()
add_executable(metadata main.cpp ../test_empty_stackmap.cpp)
add_executable(metadata-code-shape ../metadata_code_shape.cpp)
if(WIN32)
    target_sources(metadata PRIVATE windows_input.S)
    target_include_directories(metadata PRIVATE ${RUNTIME}/src/Heap/z/os/windows ${RUNTIME}/src/os/Windows)
endif()
foreach(_metadata_target metadata metadata-code-shape)
target_include_directories(${_metadata_target} PRIVATE .. ${RUNTIME}/src ${RUNTIME}/include
    ${RUNTIME}/src/Loader/BinaryFile ${RUNTIME}/src/Heap
    ${PRODUCT_BUILD}/src/Heap ${PRODUCT_BUILD}/src/Heap/Allocator
    ${PRODUCT_BUILD}/runtime-staging/include
    ${RUNTIME}/third_party/third_party_bounds_checking_function/include)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_include_directories(${_metadata_target} PRIVATE ${RUNTIME}/src/Heap/z/os/linux)
    target_link_options(${_metadata_target} PRIVATE -Wl,--exclude-libs,ALL)
endif()
target_compile_features(${_metadata_target} PRIVATE cxx_std_17)
target_compile_options(${_metadata_target} PRIVATE -fno-rtti -fexceptions -fvisibility-inlines-hidden)
target_compile_definitions(${_metadata_target} PRIVATE MRT_TESTABLE_INTERNALS=1 MRT_GC_UNIT_TESTS=1
    MRT_PRODUCT_TESTABLE_INTERNALS=1)
target_link_directories(${_metadata_target} PRIVATE ${GCV2_RUNTIME_LIB_DIR})
target_link_libraries(${_metadata_target} PRIVATE cangjie-runtime boundscheck)
set_target_properties(${_metadata_target} PROPERTIES BUILD_RPATH "${GCV2_RUNTIME_LIB_DIR}")
endforeach()

foreach(_image cj_metadata_foreign cj_metadata_owner)
  add_library(${_image} SHARED ../metadata_owner_records.cpp)
  set_target_properties(${_image} PROPERTIES PREFIX "")
endforeach()
target_compile_definitions(cj_metadata_foreign PRIVATE GC_METADATA_FOREIGN_IMAGE=1)
add_dependencies(metadata-code-shape cj_metadata_foreign cj_metadata_owner)

# Isolated existing metadata entry: no gc_unit aggregate or product source copy.
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(cj_metadata_contiguous SHARED ../metadata_owner_records.cpp)
  set_target_properties(cj_metadata_contiguous PROPERTIES PREFIX "")
  target_compile_definitions(cj_metadata_contiguous PRIVATE GC_METADATA_CONTIGUOUS_IMAGE=1)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(cj_metadata_owner PRIVATE -Wl,--section-start=.a2_boundary=0x400000)
  target_link_options(cj_metadata_contiguous PRIVATE
      -Wl,-z,max-page-size=4096
      -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/../a2_contiguous.ld
      -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/contiguous-link.map,--cref,--trace)
  set_property(TARGET cj_metadata_contiguous APPEND PROPERTY LINK_DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/../a2_contiguous.ld)
  add_custom_command(TARGET cj_metadata_contiguous POST_BUILD
      COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../a2_prefix_hole.py
          $<TARGET_FILE:cj_metadata_contiguous>
          $<TARGET_FILE_DIR:cj_metadata_contiguous>/cj_metadata_hole.so
      VERBATIM)
  endif()
  add_dependencies(metadata-code-shape cj_metadata_contiguous)
endif()
