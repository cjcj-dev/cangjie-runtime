// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_Z_VERIFY_H
#define MRT_Z_VERIFY_H
#include "Common/BaseObject.h"
#include "ObjectModel/RefField.h"
namespace MapleRuntime {
class ZForwarding;
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
void z_verify_safepoints_are_blocked();
#endif
// z_globals.hpp:78-105. The runtime has no HotSpot flag parser; retain the
// upstream flag names and defaults as startup environment options (0/1).
extern const bool ZVerifyRoots;
extern const bool ZVerifyObjects;
extern const bool ZVerifyMarking;
extern const bool ZVerifyRemembered;
extern const bool ZVerifyForwarding;
extern const bool ZVerifyOops;
class ZVerify {
public:
    static void BeforeZOperation();
    static void AfterMark();
    static void AfterWeakProcessing();
    static void BeforeRelocation(ZForwarding* forwarding);
    static void AfterRelocation(ZForwarding* forwarding);
    static void AfterScan(ZForwarding* forwarding);
    static void OnColorFlip();
    static void threads_start_processing();
private:
    static void RootsStrong(bool afterOldMark);
    static void RootsWeak();
    static void Objects(bool verifyWeaks);
    static void AfterRelocationInternal(ZForwarding* forwarding);
};
} // namespace MapleRuntime
#endif
