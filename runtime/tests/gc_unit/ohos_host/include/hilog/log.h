// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_GC_UNIT_OHOS_HOST_HILOG_LOG_H
#define MRT_GC_UNIT_OHOS_HOST_HILOG_LOG_H

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#define LOG_DEBUG 3
#define LOG_INFO 4
#define LOG_WARN 5
#define LOG_ERROR 6
#define LOG_FATAL 7
#define LOG_APP 0

// This host adapter has no hilog service or privacy filtering. Convert hilog
// conversion annotations to printf syntax; escaped percent signs stay literal.
static inline void OhosHostLog(const char* level, const char* tag, const char* format, ...)
{
    std::string hostFormat;
    for (const char* cursor = format; *cursor != '\0'; ++cursor) {
        hostFormat += *cursor;
        if (*cursor != '%') {
            continue;
        }
        if (cursor[1] == '%') {
            hostFormat += *++cursor;
        } else if (std::strncmp(cursor + 1, "{public}", 8) == 0) {
            cursor += 8;
        } else if (std::strncmp(cursor + 1, "{private}", 9) == 0) {
            cursor += 9;
        }
    }
    va_list args;
    va_start(args, format);
    flockfile(stderr);
    std::fprintf(stderr, "[%s] %s: ", level, tag);
    std::vfprintf(stderr, hostFormat.c_str(), args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    funlockfile(stderr);
    va_end(args);
}

#define OH_LOG_IsLoggable(...) 1
#define OH_LOG_DEBUG(type, ...) OhosHostLog("DEBUG", LOG_TAG, __VA_ARGS__)
#define OH_LOG_INFO(type, ...) OhosHostLog("INFO", LOG_TAG, __VA_ARGS__)
#define OH_LOG_WARN(type, ...) OhosHostLog("WARN", LOG_TAG, __VA_ARGS__)
#define OH_LOG_ERROR(type, ...) OhosHostLog("ERROR", LOG_TAG, __VA_ARGS__)
#define OH_LOG_FATAL(type, ...) OhosHostLog("FATAL", LOG_TAG, __VA_ARGS__)

#endif
