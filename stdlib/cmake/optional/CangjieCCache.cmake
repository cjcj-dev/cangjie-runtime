# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

if(CANGJIE_ENABLE_CCACHE)
    find_program(FOUND ccache)
    if(FOUND)
        # Admission must precede ccache (including cache hits and links).
        # AddCangjieSource owns the budget prefix; do not overwrite it here.
        foreach(rule RULE_LAUNCH_COMPILE RULE_LAUNCH_LINK)
            get_property(launcher GLOBAL PROPERTY ${rule})
            if(launcher)
                set_property(GLOBAL PROPERTY ${rule} "${launcher} \"${FOUND}\"")
            else()
                set_property(GLOBAL PROPERTY ${rule} "\"${FOUND}\"")
            endif()
        endforeach()
        message(STATUS "CCache found. Build with CCache.")
    endif()
endif()
