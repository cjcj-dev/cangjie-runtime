# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "VerifyRememberedSet.cpp"
    "VerifyRoots.cpp"
    "VerifyHeap.cpp"
    "MarkCompleteVerify.cpp"
    "SurvNodeDiag.cpp"
    "StatHealDiag.cpp"
    "VerifyOption.cpp"
    "Zap.cpp"
    "VerifyRegions.cpp"
    "MinorGCALot.cpp"
    "TraceClear.cpp"
    "TagReuseProbe.cpp"
    "MarkWhyProbe.cpp"
    "StackFrameOracle.cpp"
    "StackWatermarkOracle.cpp"
    "StackExposureOracle.cpp"
    "DiagGate.cpp"
    "VerifyPhase.cpp"
    "MutatorRelocate.cpp"
    "GarbRegionDiag.cpp"
    "PermWhoAdmit.cpp"
    "HealPairDiag.cpp"
    "ZgcSelfHealDiag.cpp"
    "ZgcInvariants.cpp"
    "M0ExitDiagnostics.cpp"
    "M0Correlation.cpp"
    "HealCoverage.cpp"
    "Stw2CurrentAudit.cpp"
    "CsetEmptyWho.cpp"
    "FillerZeroDiag.cpp"
    "HoleWhoDiag.cpp"
    "FromPageDetachCheck.cpp"
    "ColourCensus.cpp"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
add_library(Verify STATIC ${SRC_LIST})
