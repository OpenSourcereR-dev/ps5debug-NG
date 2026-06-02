// SPDX-License-Identifier: GPL-3.0-only

#include "protocol.h"
#include "sdk_shim.h"
#include "net.h"
#include "kern_rw_fast.h"
#include "proc_field_offsets.h"
#include <stdarg.h>

extern int find_titleid(const uint8_t *buf, size_t len, char *out);
extern int find_contentid(const uint8_t *buf, size_t len, char *out, size_t out_size);

#define PROC_NEXT_OFFSET           0x00
#define PROC_SELFINFO_NAME_SIZE    32

struct cmd_console_notify_packet {
    uint32_t messageType;
    uint32_t length;
} __attribute__((packed));

struct cmd_console_print_packet {
    uint32_t length;
} __attribute__((packed));

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int console_reboot_handle(int fd, struct cmd_packet *packet) {
    (void)fd; (void)packet;
    sceKernelReboot();
    return 0;
}

int console_end_handle(int fd, struct cmd_packet *packet) {
    (void)packet;
    net_send_int32(fd, CMD_SUCCESS);
    return 0;
}

int console_print_handle(int fd, struct cmd_packet *packet) {
    struct cmd_console_print_packet *pp = (struct cmd_console_print_packet *)packet->data;
    if (!pp) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }
    void *data = net_alloc_buffer(pp->length);
    if (!data) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }
    memset(data, 0, pp->length);
    net_recv_all(fd, data, (int)pp->length, 1);
    klog_puts((const char *)data);
    net_send_int32(fd, CMD_SUCCESS);
    free(data);
    return 0;
}

int console_notify_handle(int fd, struct cmd_packet *packet) {
    struct cmd_console_notify_packet *np = (struct cmd_console_notify_packet *)packet->data;
    if (!np) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }
    void *data = net_alloc_buffer(np->length);
    if (!data) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }
    memset(data, 0, np->length);
    net_recv_all(fd, data, (int)np->length, 1);

    notify_request_t req;
    memset(&req, 0, sizeof(req));
    uint32_t copy = np->length;
    if (copy >= sizeof(req.message)) copy = sizeof(req.message) - 1;
    memcpy(req.message, data, copy);
    req.message[copy] = 0;
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);

    net_send_int32(fd, CMD_SUCCESS);
    free(data);
    return 0;
}

int console_info_handle(int fd, struct cmd_packet *packet) {
    (void)packet;
    net_send_int32(fd, CMD_SUCCESS);
    return 0;
}

static int sfo_read_file(const char *path, uint8_t *buf, size_t bufsz, size_t *out_len) {
    int fdf = open(path, 0, 0);
    if (fdf < 0) return -1;
    size_t total = 0;
    while (total < bufsz) {
        long n = read(fdf, buf + total, bufsz - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fdf);
    if (out_len) *out_len = total;
    return 0;
}

static int sfo_get_string(const uint8_t *sfo, size_t sfo_len,
                          const char *want_key, char *out, size_t out_max) {
    if (out_max == 0) return -1;
    out[0] = '\0';
    if (sfo_len < 20) return -1;
    if (sfo[0] != 0 || sfo[1] != 'P' || sfo[2] != 'S' || sfo[3] != 'F') return -1;
    uint32_t key_table  = *(const uint32_t *)(sfo + 8);
    uint32_t data_table = *(const uint32_t *)(sfo + 12);
    uint32_t nentries   = *(const uint32_t *)(sfo + 16);
    if (key_table  > sfo_len) return -1;
    if (data_table > sfo_len) return -1;
    size_t want_len = strlen(want_key);
    for (uint32_t i = 0; i < nentries; i++) {
        size_t eoff = 20 + (size_t)i * 16;
        if (eoff + 16 > sfo_len) return -1;
        const uint8_t *e = sfo + eoff;
        uint16_t key_off   = *(const uint16_t *)(e + 0);
        uint32_t param_len = *(const uint32_t *)(e + 4);
        uint32_t data_off  = *(const uint32_t *)(e + 12);
        size_t key_abs = (size_t)key_table + key_off;
        if (key_abs + want_len + 1 > sfo_len) continue;
        if (memcmp(sfo + key_abs, want_key, want_len) != 0) continue;
        if (sfo[key_abs + want_len] != '\0') continue;
        size_t data_abs = (size_t)data_table + data_off;
        if (data_abs + param_len > sfo_len) return -1;
        size_t copy = param_len;
        if (copy >= out_max) copy = out_max - 1;
        memcpy(out, sfo + data_abs, copy);
        out[copy] = '\0';
        while (copy > 0 && out[copy - 1] == '\0') copy--;
        out[copy] = '\0';
        return 0;
    }
    return -2;
}

struct cmd_console_foreground_app_response {
    uint32_t pid;
    char     titleid[16];
    char     contentid[64];
    char     name[40];
    char     app_ver[16];
} __attribute__((packed));

static int json_get_string(const uint8_t *json, size_t json_len,
                           const char *key, char *out, size_t out_max) {
    if (out_max == 0) return -1;
    out[0] = '\0';
    char needle[64];
    int nl = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nl <= 0 || (size_t)nl >= sizeof(needle)) return -1;
    size_t need_len = (size_t)nl;
    const uint8_t *found = NULL;
    for (size_t i = 0; i + need_len <= json_len; i++) {
        if (memcmp(json + i, needle, need_len) == 0) {
            found = json + i + need_len;
            break;
        }
    }
    if (!found) return -2;
    const uint8_t *end = json + json_len;
    while (found < end && (*found == ' ' || *found == ':' || *found == '\t'
                        || *found == '\n' || *found == '\r')) {
        found++;
    }
    if (found >= end || *found != '"') return -1;
    found++;
    size_t copy = 0;
    while (found < end && *found != '"' && copy < out_max - 1) {
        out[copy++] = (char)(*found++);
    }
    out[copy] = '\0';
    return 0;
}

int console_foreground_app_handle(int fd, struct cmd_packet *packet) {
    (void)packet;
    struct cmd_console_foreground_app_response resp;
    memset(&resp, 0, sizeof(resp));

    struct proc_field_offsets off;
    if (proc_get_field_offsets(&off) != 0) {
        net_send_int32(fd, CMD_SUCCESS);
        net_send_all(fd, &resp, sizeof(resp));
        return 0;
    }

    intptr_t allproc_head = (intptr_t)KERNEL_ADDRESS_ALLPROC;
    intptr_t kproc = 0;
    if (kernel_copyout_fast(allproc_head, &kproc, sizeof(kproc)) != 0) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    intptr_t found_proc = 0;
    int32_t  found_pid = 0;
    intptr_t cur = kproc;
    for (uint32_t guard = 0; cur != 0 && guard < 0x1000; guard++) {
        char nm[PROC_SELFINFO_NAME_SIZE + 1];
        nm[PROC_SELFINFO_NAME_SIZE] = 0;
        if (kernel_copyout_fast(cur + off.name, nm, PROC_SELFINFO_NAME_SIZE) == 0
            && nm[0] != 0
            && strncmp(nm, "eboot.bin", 9) == 0
            && (nm[9] == '\0' || nm[9] == ' ')) {
            int32_t pid_val = 0;
            kernel_copyout_fast(cur + KERNEL_OFFSET_PROC_P_PID, &pid_val, sizeof(pid_val));
            if (pid_val > 0) {
                found_proc = cur;
                found_pid  = pid_val;
                break;
            }
        }
        intptr_t next = 0;
        if (kernel_copyout_fast(cur + PROC_NEXT_OFFSET, &next, sizeof(next)) != 0) break;
        cur = next;
    }

    if (found_proc == 0) {
        net_send_int32(fd, CMD_SUCCESS);
        net_send_all(fd, &resp, sizeof(resp));
        return 0;
    }

    void *proc_buf = kernel_get_proc_struct_fast((uint32_t)found_pid);
    if (!proc_buf) {
        resp.pid = (uint32_t)found_pid;
        memcpy(resp.name, "eboot.bin", 9);
        net_send_int32(fd, CMD_SUCCESS);
        net_send_all(fd, &resp, sizeof(resp));
        return 0;
    }
    const uint8_t *p = (const uint8_t *)proc_buf;

    resp.pid = (uint32_t)found_pid;
    memcpy(resp.name, p + off.name, PROC_SELFINFO_NAME_SIZE);

    if (find_contentid(p + 0x440, 0x200, resp.contentid, sizeof(resp.contentid)) != 0) {
        memcpy(resp.contentid, p + off.contentid, sizeof(resp.contentid));
    }

    char titleid_z[17];
    memset(titleid_z, 0, sizeof(titleid_z));
    if (find_titleid(p + 0x440, 0x200, titleid_z) != 0) {
        memcpy(titleid_z, p + off.titleid, 16);
        titleid_z[16] = '\0';
    }
    memcpy(resp.titleid, titleid_z, sizeof(resp.titleid));

    char meta_dir[96];
    snprintf(meta_dir, sizeof(meta_dir), "/system_data/priv/appmeta/%s", titleid_z);

    char sfo_path[120];
    char json_path[120];
    snprintf(sfo_path,  sizeof(sfo_path),  "%s/param.sfo",  meta_dir);
    snprintf(json_path, sizeof(json_path), "%s/param.json", meta_dir);

    uint8_t *meta_buf = (uint8_t *)net_alloc_buffer(8192);
    if (meta_buf) {
        size_t meta_len = 0;
        char ver_a[16] = {0}, ver_b[16] = {0};
        const char *pick = NULL;

        if (sfo_read_file(sfo_path, meta_buf, 8192, &meta_len) == 0 && meta_len >= 20
            && meta_buf[0] == 0 && meta_buf[1] == 'P'
            && meta_buf[2] == 'S' && meta_buf[3] == 'F') {
            sfo_get_string(meta_buf, meta_len, "APP_VER", ver_a, sizeof(ver_a));
            sfo_get_string(meta_buf, meta_len, "VERSION", ver_b, sizeof(ver_b));
            if (ver_a[0] && ver_b[0]) pick = (strcmp(ver_a, ver_b) >= 0) ? ver_a : ver_b;
            else if (ver_a[0])        pick = ver_a;
            else if (ver_b[0])        pick = ver_b;
        }

        if (!pick) {
            meta_len = 0;
            if (sfo_read_file(json_path, meta_buf, 8192, &meta_len) == 0 && meta_len >= 2
                && meta_buf[0] == '{') {
                json_get_string(meta_buf, meta_len, "contentVersion",       ver_a, sizeof(ver_a));
                json_get_string(meta_buf, meta_len, "originContentVersion", ver_b, sizeof(ver_b));
                if (ver_a[0] && ver_b[0]) pick = (strcmp(ver_a, ver_b) >= 0) ? ver_a : ver_b;
                else if (ver_a[0])        pick = ver_a;
                else if (ver_b[0])        pick = ver_b;
                if (!pick) {
                    json_get_string(meta_buf, meta_len, "masterVersion", ver_a, sizeof(ver_a));
                    if (ver_a[0]) pick = ver_a;
                }
            }
        }

        if (pick) {
            size_t plen = strlen(pick);
            if (plen >= sizeof(resp.app_ver)) plen = sizeof(resp.app_ver) - 1;
            memcpy(resp.app_ver, pick, plen);
            resp.app_ver[plen] = '\0';
        }
        free(meta_buf);
    }

    free(proc_buf);

    net_send_int32(fd, CMD_SUCCESS);
    net_send_all(fd, &resp, sizeof(resp));
    return 0;
}

int console_handle(int fd, struct cmd_packet *packet) {
    switch (packet->cmd) {
        case 0xBDDD0001u: return console_reboot_handle(fd, packet);
        case 0xBDDD0002u: return console_end_handle(fd, packet);
        case 0xBDDD0003u: return console_print_handle(fd, packet);
        case 0xBDDD0004u: return console_notify_handle(fd, packet);
        case 0xBDDD0005u: return console_info_handle(fd, packet);
        case 0xBDDD0006u: return console_foreground_app_handle(fd, packet);
        default:      return 1;
    }
}

__attribute__((noinline))
void klog_format_log(const char *fmt, ...)
{
    char formatted[300];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(formatted, fmt, ap);
    va_end(ap);
    printf("[ps5debug] %s\n", formatted);
}

struct klog_sin {
    uint8_t  sin_len;
    uint8_t  sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
};

int g_klog_listen_fd = -1;

__attribute__((noinline))
void *klog_server_thread(void *arg)
{
    (void)arg;

    int s = sceNetSocket((const char *)0, 2, 1, 0);
    if (s < 0) return (void *)0;

    int one = 1;
    sceNetSetsockopt(s, 0xFFFF, 0x0004, &one, 4);
    sceNetSetsockopt(s, 0xFFFF, 0x0200, &one, 4);
    sceNetSetsockopt(s, 0xFFFF, 0x0008, &one, 4);

    struct klog_sin sin;
    memset(&sin, 0, 16);
    sin.sin_family = 2;
    sin.sin_port   = 0xA00C;
    sin.sin_addr   = 0;

    if (sceNetBind(s, &sin, 16) < 0)   return (void *)0;
    if (sceNetListen(s, 5) < 0)        return (void *)0;

    g_klog_listen_fd = s;

    char rbuf[0x1000];
    struct timespec ts;

    for (;;) {
        scePthreadYield();
        int connfd = sceNetAccept(s, (void *)0, (void *)0);
        if (connfd < 0) {

            int err = *sceNetErrnoLoc();
            if (err == 0xA3 || err == EBADF) {
                sceNetSocketClose(s);
                g_klog_listen_fd = -1;
                return (void *)0;
            }
            continue;
        }
        if (connfd == 0) continue;

        int32_t nosig = 1;
        sceNetSetsockopt(connfd, 0xFFFF, 0x0800, &nosig, 4);

        int klog_fd = open("/dev/klog", 0, 0);

        if (klog_fd == -1) {
            write(connfd, "Cannot open /dev/klog\n", 0x17);
            return (void *)0;
        }

        write(connfd, "Successfully opened klog!\n", 0x0A);

        for (;;) {
            ts.tv_sec  = 0;
            ts.tv_nsec = 100000000L;
            nanosleep(&ts, (struct timespec *)0);

            int err_val = 0;
            unsigned int err_len = 4;
            int gso = sceNetGetsockopt(connfd, 0xFFFF, 0x1007, &err_val, &err_len);
            if ((gso | err_val) != 0) break;

            long n = read(klog_fd, rbuf, 0xFFF);
            if (n > 0) {
                rbuf[n] = 0;
                long expect = n + 1;
                if (write(connfd, rbuf, (unsigned long)expect) != expect)
                    break;
            }

        }

        klog_format_log("klog client disconnected");
        close(klog_fd);
        close(connfd);
    }
}
