/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

/*
 * WASI Preview 1 has no fork/exec, waitpid, kill or passwd database. This file replaces
 * process_ffi_unix.c for wasm32-wasi: starting, waiting for or terminating a process fails with
 * ENOTSUP, so std.process raises ProcessException, while the current-process queries and the
 * descriptor helpers keep working. The module is the only process and reports pid 1.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "process_ffi_unix.h"

extern char** environ;

#define WASI_PID (1)
#define INVALID_PID (-1)
#define INVALID_FD (-1)

extern ProcessStartInfo* CJ_OS_CreateProcessStartInfo(void)
{
    return (ProcessStartInfo*)malloc(sizeof(ProcessStartInfo));
}

extern void CJ_OS_FreeProcessStartInfo(ProcessStartInfo* startInfo)
{
    free(startInfo);
}

extern int64_t CJ_OS_FileRead(int32_t fd, char* buffer, size_t maxLen)
{
    return (int64_t)read(fd, buffer, maxLen);
}

extern bool CJ_OS_FileWrite(int32_t fd, const char* buffer, size_t maxLen)
{
    const char* ptr = buffer;
    size_t remainingLen = maxLen;
    while (remainingLen > 0) {
        ssize_t writeSize = write(fd, ptr, remainingLen);
        if (writeSize <= 0) {
            break;
        }
        remainingLen -= (size_t)writeSize;
        ptr += writeSize;
    }
    return remainingLen == 0;
}

extern int64_t CJ_OS_CloseFile(int32_t fd)
{
    return (int64_t)close(fd);
}

void FreeTwoDimensionalArray(char** strArray)
{
    if (!strArray) {
        return;
    }
    for (char** p = strArray; *p; ++p) {
        free(*p);
    }
    free(strArray);
}

extern ProcessRtnData* CJ_OS_StartProcess(ProcessStartInfo* info)
{
    (void)info;
    ProcessRtnData* processData = (ProcessRtnData*)calloc(1, sizeof(ProcessRtnData));
    if (processData == NULL) {
        return NULL;
    }
    processData->pid = INVALID_PID;
    processData->stdIn = INVALID_FD;
    processData->stdOut = INVALID_FD;
    processData->stdErr = INVALID_FD;
    processData->errCode = ENOTSUP;
    processData->errMessage = strdup("starting a process is not supported on WASI Preview 1");
    return processData;
}

extern int64_t CJ_OS_WaitSubProcessExit(int32_t pid)
{
    (void)pid;
    errno = ENOTSUP;
    return -1;
}

extern int32_t CJ_OS_WaitSubProcessExitNonBlocking(int32_t pid, int64_t* exitCode)
{
    (void)pid;
    (void)exitCode;
    errno = ENOTSUP;
    return -1;
}

extern int32_t CJ_OS_Terminate(int32_t pid, bool force)
{
    (void)pid;
    (void)force;
    errno = ENOTSUP;
    return -1;
}

extern int32_t CJ_OS_GetCurrentPid(void)
{
    return WASI_PID;
}

extern void CJ_OS_FreeProcessRtnData(ProcessRtnData* data)
{
    if (data != NULL) {
        free(data->errMessage);
        data->errMessage = NULL;
        free(data);
    }
}

extern char* CJ_OS_HomeDir(void)
{
    return getenv("HOME");
}

char** GetCurrentProcessEnvironment(int32_t pid)
{
    if (pid != WASI_PID || environ == NULL) {
        return NULL;
    }
    size_t envCount = 0;
    for (char** envPtr = environ; *envPtr != NULL; ++envPtr) {
        envCount++;
    }
    char** environment = calloc(envCount + 1, sizeof(char*));
    if (!environment) {
        return NULL;
    }
    for (size_t i = 0; i < envCount; i++) {
        environment[i] = strdup(environ[i]);
    }
    return environment;
}
