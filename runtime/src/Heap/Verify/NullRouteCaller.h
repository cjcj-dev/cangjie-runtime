// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_NULL_ROUTE_CALLER_H
#define MRT_NULL_ROUTE_CALLER_H

// routecaller: TLS tag for ForwardObject → GetRoute null-route diag.
// Gate: MRT_GCV2_NULLROUTE_DIAG=1 (same as RegionInfo::GetRoute sample).
// Default off: tag is written only when diag is armed; readers print "none" if unset.

namespace MapleRuntime {
namespace NullRouteCaller {

inline thread_local const char* g_tag = nullptr;

class ScopedTag {
public:
    explicit ScopedTag(const char* tag) : prev_(g_tag)
    {
        g_tag = tag;
    }
    ~ScopedTag() { g_tag = prev_; }
    ScopedTag(const ScopedTag&) = delete;
    ScopedTag& operator=(const ScopedTag&) = delete;

private:
    const char* prev_;
};

inline const char* Current()
{
    return g_tag != nullptr ? g_tag : "none";
}

} // namespace NullRouteCaller
} // namespace MapleRuntime

#endif // MRT_NULL_ROUTE_CALLER_H
