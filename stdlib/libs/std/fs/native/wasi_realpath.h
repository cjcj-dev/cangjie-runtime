/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

#ifndef CJ_WASI_REALPATH_H
#define CJ_WASI_REALPATH_H

#ifdef __wasi__
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * WASI Preview 1 (wasi-libc) does not provide realpath(3). Resolve "." and ".." lexically
 * against the emulated working directory and require the result to exist. Symbolic links
 * are not expanded. A relative path stays relative while the working directory is "/",
 * so it is still looked up through the preopened directories.
 */
static inline char* CJ_WasiRealPath(const char* path, char* resolved)
{
    char buf[PATH_MAX];
    size_t out = 0;
    int absolute;
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }
    absolute = path[0] == '/';
    buf[0] = '\0';
    if (!absolute) {
        if (getcwd(buf, sizeof(buf)) == NULL) {
            return NULL;
        }
        out = strlen(buf);
        if (out == 1 && buf[0] == '/') {
            out = 0; /* keep the result relative */
            buf[0] = '\0';
        } else {
            absolute = 1;
        }
    }
    const char* p = path;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char* s = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t len = (size_t)(p - s);
        if (len == 1 && s[0] == '.') {
            continue;
        }
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (out > 0 && buf[out - 1] != '/') {
                out--;
            }
            if (out > 0) {
                out--;
            }
            buf[out] = '\0';
            continue;
        }
        size_t sep = (absolute || out > 0) ? 1 : 0;
        if (out + sep + len + 1 > sizeof(buf)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (sep) {
            buf[out++] = '/';
        }
        memcpy(buf + out, s, len);
        out += len;
        buf[out] = '\0';
    }
    if (out == 0) {
        buf[0] = absolute ? '/' : '.';
        buf[1] = '\0';
        out = 1;
    }
    struct stat st;
    if (stat(buf, &st) != 0) {
        return NULL;
    }
    if (resolved == NULL) {
        resolved = (char*)malloc(out + 1);
        if (resolved == NULL) {
            errno = ENOMEM;
            return NULL;
        }
    }
    memcpy(resolved, buf, out + 1);
    return resolved;
}

#define realpath(path, resolved) CJ_WasiRealPath((path), (resolved))
#endif /* __wasi__ */

#endif /* CJ_WASI_REALPATH_H */
