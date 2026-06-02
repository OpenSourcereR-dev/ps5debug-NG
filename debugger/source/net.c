// SPDX-License-Identifier: GPL-3.0-only

#include "protocol.h"
#include "sdk_shim.h"
#include "net.h"

int net_send_all(int fd, const void *buf, int len) {
    const char *p = (const char *)buf;
    int left = len;
    while (left > 0) {
        int chunk = (left > NET_MAX_LENGTH) ? NET_MAX_LENGTH : left;

        ssize_t n = send(fd, p, chunk, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        left -= (int)n;
    }
    return 0;
}

int net_recv_all(int fd, void *buf, int len, int force) {
    char *p = (char *)buf;
    int left = len;
    while (left > 0) {
        int chunk = (left > NET_MAX_LENGTH) ? NET_MAX_LENGTH : left;
        int flags = force ? MSG_WAITALL : 0;
        ssize_t n = recv(fd, p, chunk, flags);
        if (n <= 0) return -1;
        p += n;
        left -= (int)n;
    }
    return 0;
}

static inline uint32_t bitswap32(uint32_t x) {
    return ((x >> 1) & 0x55555555u) | ((x << 1) & 0xAAAAAAAAu);
}

int net_send_int32(int fd, uint32_t value) {
    uint32_t wire = bitswap32(value);
    return net_send_all(fd, &wire, sizeof(wire));
}

extern long __crt_syscall(long sysno, ...);
int net_select(int fd, void *readfds, void *writefds, void *exceptfds, void *timeout) {
    return (int)__crt_syscall(93, fd, readfds, writefds, exceptfds, timeout);
}

int net_get_ip_address(char *out) {
    if (sceNetCtlInit() < 0) return -1;
    uint8_t info[0x100];
    int rc = sceNetCtlGetInfo(0xE, info);
    if (rc < 0) return -1;
    memcpy(out, info, 0x10);
    sceNetCtlTerm();
    return rc;
}

void *net_alloc_buffer(uint32_t size) {
    if (size == 0) return NULL;
    void *p = malloc(size);
    if (!p) return NULL;

    volatile char *cp = (volatile char *)p;
    for (uint32_t i = 0; i < size; i += 0x1000) cp[i] = 0;
    if (size > 0) cp[size - 1] = 0;
    return p;
}
