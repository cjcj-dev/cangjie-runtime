/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

/*
 * WASI Preview 1 has no BSD socket API (no socket/connect/bind/getaddrinfo). This file replaces
 * utils.c and socket_buffer.c for wasm32-wasi: every socket entry point std.net calls fails with
 * ENOTSUP, so std.net raises its socket exception, and the address helpers keep working.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct SockAddr {
    void* sockaddr;
    uint32_t addrLen;
};

void* CJ_RawAddressCreate(void* arr, uint32_t size)
{
    struct SockAddr* addr = malloc(sizeof(struct SockAddr));
    if (addr == NULL) {
        return NULL;
    }
    addr->sockaddr = malloc(size);
    if (addr->sockaddr == NULL) {
        free(addr);
        return NULL;
    }
    memcpy(addr->sockaddr, arr, size);
    addr->addrLen = size;
    return addr;
}

int32_t CJ_GetOptionInt(int64_t sock, int32_t level, int32_t optname)
{
    (void)sock; (void)level; (void)optname; errno = ENOTSUP; return -1;
}

// Nothing to initialize: every socket operation reports ENOTSUP.
int32_t CJ_MRT_DomainsockInit(void)
{
    return 0;
}

// Nothing to initialize: every socket operation reports ENOTSUP.
int32_t CJ_MRT_RawsockInit(void)
{
    return 0;
}

int64_t CJ_MRT_SockAccept(int64_t sock, void* addr)
{
    (void)sock; (void)addr; errno = ENOTSUP; return -1;
}

int64_t CJ_MRT_SockAcceptTimeout(int64_t sock, void* addr, uint64_t timeout)
{
    (void)sock; (void)addr; (void)timeout; errno = ENOTSUP; return -1;
}

int64_t CJ_MRT_SockBind(int64_t sock, void* addr, int32_t backlog)
{
    (void)sock; (void)addr; (void)backlog; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockClose(int64_t sock)
{
    (void)sock; errno = ENOTSUP; return -1;
}

int64_t CJ_MRT_SockConnect(int64_t sock, void* local_addr, void* peer_addr)
{
    (void)sock; (void)local_addr; (void)peer_addr; errno = ENOTSUP; return -1;
}

int64_t CJ_MRT_SockCreate(int32_t domain, int32_t sock_type, int32_t protocol, const char* net)
{
    (void)domain; (void)sock_type; (void)protocol; (void)net; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockDisconnect(int64_t sock, bool isIpv6)
{
    (void)sock; (void)isIpv6; errno = ENOTSUP; return -1;
}

int64_t CJ_MRT_SockListen(int64_t sockfd, int32_t backlog)
{
    (void)sockfd; (void)backlog; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockLocalAddrGet(int64_t sock, void* addr)
{
    (void)sock; (void)addr; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockPeerAddrGet(int64_t sock, void* addr)
{
    (void)sock; (void)addr; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockRecv(int64_t sock, void* buf, uint32_t length, int32_t flags)
{
    (void)sock; (void)buf; (void)length; (void)flags; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockRecvNonBlock(int64_t handle, void* buf, uint32_t len, int32_t flags)
{
    (void)handle; (void)buf; (void)len; (void)flags; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockRecvTimeout(int64_t sock, void* buf, uint32_t length, int32_t flags, uint64_t timeout)
{
    (void)sock; (void)buf; (void)length; (void)flags; (void)timeout; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockSend(int64_t sock, void* buf, uint32_t length, int32_t flags)
{
    (void)sock; (void)buf; (void)length; (void)flags; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockSendNonBlock(int64_t handle, void* buf, uint32_t len, int32_t flags)
{
    (void)handle; (void)buf; (void)len; (void)flags; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockSendTimeout(int64_t sock, void* buf, uint32_t length, int32_t flags, uint64_t timeout)
{
    (void)sock; (void)buf; (void)length; (void)flags; (void)timeout; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockShutdown(int64_t sock)
{
    (void)sock; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockWaitRecv(int64_t handle)
{
    (void)handle; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockWaitRecvTimeout(int64_t handle, uint64_t timeout)
{
    (void)handle; (void)timeout; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockWaitSend(int64_t handle)
{
    (void)handle; errno = ENOTSUP; return -1;
}

int32_t CJ_MRT_SockWaitSendTimeout(int64_t handle, uint64_t timeout)
{
    (void)handle; (void)timeout; errno = ENOTSUP; return -1;
}

// Nothing to initialize: every socket operation reports ENOTSUP.
int32_t CJ_MRT_TcpsockInit(void)
{
    return 0;
}

// Nothing to initialize: every socket operation reports ENOTSUP.
int32_t CJ_MRT_UdpsockInit(void)
{
    return 0;
}

void CJ_RawAddressDestroy(void* addrPtr)
{
    struct SockAddr* addr = (struct SockAddr*)addrPtr;
    if (addr != NULL) {
        free(addr->sockaddr);
        free(addr);
    }
}

uint32_t CJ_RawAddressLenGet(void* addrPtr)
{
    return ((struct SockAddr*)addrPtr)->addrLen;
}

int32_t CJ_SOCKET_BufferClose(void* sockBuf, int64_t handle)
{
    (void)sockBuf; (void)handle; errno = ENOTSUP; return -1;
}

void* CJ_SOCKET_BufferInit(int64_t handle, int32_t rBufSize, int32_t wBufSize)
{
    (void)handle; (void)rBufSize; (void)wBufSize; errno = ENOTSUP; return NULL;
}

int32_t CJ_SOCKET_BufferRCopy(void* sockBuf, void* arrBuf, int64_t bufLen, int32_t copyLen)
{
    (void)sockBuf; (void)arrBuf; (void)bufLen; (void)copyLen; errno = ENOTSUP; return -1;
}

int32_t CJ_SOCKET_BufferWCopy(void* sockBuf, void* arrBuf, int64_t bufLen, int32_t copyLen)
{
    (void)sockBuf; (void)arrBuf; (void)bufLen; (void)copyLen; errno = ENOTSUP; return -1;
}

void* CJ_SOCKET_GetAddrFromAddrinfo(void* info)
{
    (void)info; errno = ENOTSUP; return NULL;
}

const char* CJ_SOCKET_GetErrMessage(int32_t err)
{
    (void)err;
    return strdup("sockets are not supported on WASI Preview 1");
}

int32_t CJ_SetOptionInt(int64_t sock, int32_t level, int32_t optname, int32_t optval)
{
    (void)sock; (void)level; (void)optname; (void)optval; errno = ENOTSUP; return -1;
}

int32_t CJ_SockErrnoGet(void)
{
    return errno;
}

int32_t CJ_SockKeepAliveSet(int64_t sock, void* cfg)
{
    (void)sock; (void)cfg; errno = ENOTSUP; return -1;
}

int32_t CJ_SockOptionSet(int64_t sock, int32_t level, int32_t optname, void* optval, int32_t oplen)
{
    (void)sock; (void)level; (void)optname; (void)optval; (void)oplen; errno = ENOTSUP; return -1;
}
