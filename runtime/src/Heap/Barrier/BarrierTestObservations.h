// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#if defined(MRT_TESTABLE_INTERNALS)
std::function<void(Barrier::FieldMarkKind, RefField<>&, zpointer, zaddress)> Barrier::testFieldMarkResult;
#endif
