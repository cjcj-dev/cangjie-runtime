// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RELOCATION_SET_INLINE_H
#define MRT_RELOCATION_SET_INLINE_H

// The from-list reinstall hook was deleted with the page lists (issue 710):
// the relocation set is installed from the selector at
// ZGeneration::select_relocation_set (zGeneration.cpp:254), never from a
// page list (zRelocationSet.cpp:52-60).

#endif
