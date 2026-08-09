// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_SIGNAL_SIGNALUTILS_H
#define MRT_SIGNAL_SIGNALUTILS_H

#include <csignal>

#include "Base/CString.h"
#include "Base/Types.h"
#include "Base/Log.h"
#include "ucontext.h"

namespace MapleRuntime {
// Always available (release + debug): si_code names are required for machine-readable
// crash signatures (GC_DIAGNOSTICS_PLAN Phase 1). Not gated on MRT_DEBUG.
const char* SignalCodeName(int sig, int code);
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
FixedCString PrintSignalInfo(const siginfo_t& info);
#endif
// Architecture dependent
Uptr GetPCFromUContext(const ucontext_t& context);
Uptr GetFAFromUContext(const ucontext_t& context);

// Best-effort register snapshot for crash records. Writes "reg:0x..,..." into buf.
// AS-oriented: stack only, no allocation. Returns bytes written excluding NUL.
int FormatRegsFromUContext(const ucontext_t& ucontext, char* buf, size_t bufSize);
} // namespace MapleRuntime

#endif // MRT_SIGNAL_SIGNALUTILS_H
