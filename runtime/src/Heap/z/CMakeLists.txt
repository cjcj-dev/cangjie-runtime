# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "../Collector/Generation.cpp"
    "WCollector.cpp"
    "zGeneration.cpp"
    "zMark.cpp"
    "zRelocate.cpp"
    "zJNICritical.cpp"
    "zRemembered.cpp"
    "zRelocationSet.cpp"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
add_library(WCollector STATIC ${SRC_LIST})
