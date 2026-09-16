# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "Log.cpp"
    "SysCall.cpp"
    "CString.cpp"
    "FixedCString.cpp"
    "TimeUtils.cpp"
    "LogFile.cpp"
    "MemUtils.cpp"
    "../Heap/z/zStat.cpp"
    "../Heap/z/zServiceability.cpp"
)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND NOT WIN32)
    list(APPEND SRC_LIST "Copy_aarch64.S")
endif ()
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
add_library(Base STATIC ${SRC_LIST})
