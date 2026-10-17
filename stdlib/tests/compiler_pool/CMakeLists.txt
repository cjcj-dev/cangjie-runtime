# A real native graph using the production Cangjie custom commands.
cmake_minimum_required(VERSION 3.16.5)
project(compiler_pool NONE)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/modules/AddCangjieSource.cmake")
set(CANGJIE_CODEGEN_CJNATIVE_BACKEND ON)
set(TARGET_TRIPLE_DIRECTORY_PREFIX linux_x86_64)
set(CMAKE_BUILD_TYPE Release)
set(TRIPLE x86_64-linux-gnu)
foreach(package left right join)
    set(dependencies)
    if(package STREQUAL "join")
        set(dependencies poolleft poolright)
    endif()
    add_cangjie_library(pool${package}
        IS_STDLIB IS_PACKAGE IS_CJNATIVE_BACKEND NO_INSTALL
        PACKAGE_NAME pool${package}
        SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/${package}"
        DEPENDS ${dependencies})
endforeach()
