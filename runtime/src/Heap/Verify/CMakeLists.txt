# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "VerifyRememberedSet.cpp"
    "VerifyRoots.cpp"
    "VerifyHeap.cpp"
    "VerifyOption.cpp"
    "Zap.cpp"
    "VerifyRegions.cpp"
    "MinorGCALot.cpp"
    "RemsetPhaseProbe.cpp"
    "DiffPathExplainer.cpp"
    "TraceClear.cpp"
    "TagReuseProbe.cpp"
    "MarkWhyProbe.cpp"
    "StackFrameOracle.cpp"
    "StackWatermarkOracle.cpp"
    "StackExposureOracle.cpp"
    "PlainCensus.cpp"
    "IdleEdgeDiag.cpp"
    "PromoteFillDiag.cpp"
    "EatArmDiag.cpp"
    "FysDesignDiag.cpp"
    "DiagGate.cpp"
    "RouteDom.cpp"
    "SealCheck.cpp"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
add_library(Verify STATIC ${SRC_LIST})
