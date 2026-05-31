// SPDX-License-Identifier: GPL-3.0-only

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "sdk_shim.h"
#include "kern_rw_fast.h"

#define PROC_NEXT_OFFSET           0x00

#define PROC_SELFINFO_NAME_OFFSET \
    (((kernel_get_fw_version() & 0xffff0000u) >= 0x12000000u) ? 0x5E4u : \
     ((kernel_get_fw_version() & 0xffff0000u) >= 0x10000000u) ? 0x5DCu : \
     ((kernel_get_fw_version() & 0xffff0000u) >= 0x7000000u)  ? 0x5D4u : \
     ((kernel_get_fw_version() & 0xffff0000u) >= 0x6000000u)  ? 0x5C4u : \
                                                                0x59Cu)
															  
#define PROC_SELFINFO_NAME_SIZE    32
#define PROC_LIST_LIMIT            0x1000

int find_proc_pid_by_name(const char *name)
{
    if (!name) return -1;

    intptr_t kproc = 0;
    if (kernel_copyout_fast((intptr_t)KERNEL_ADDRESS_ALLPROC,
                            &kproc, sizeof(kproc)) != 0)
        return -1;

    intptr_t cur = kproc;
    char proc_name[PROC_SELFINFO_NAME_SIZE];

    for (uint32_t i = 0; i < PROC_LIST_LIMIT && cur != 0; i++) {
        memset(proc_name, 0, sizeof(proc_name));
        if (kernel_copyout_fast(cur + PROC_SELFINFO_NAME_OFFSET,
                                proc_name, PROC_SELFINFO_NAME_SIZE) == 0
            && proc_name[PROC_SELFINFO_NAME_SIZE - 1] == 0
            && strcmp(proc_name, name) == 0)
        {
            int32_t pid = -1;
            if (kernel_copyout_fast(cur + KERNEL_OFFSET_PROC_P_PID,
                                    &pid, sizeof(pid)) != 0)
                return -1;
            return (int)pid;
        }

        intptr_t next = 0;
        if (kernel_copyout_fast(cur + PROC_NEXT_OFFSET,
                                &next, sizeof(next)) != 0)
            return -1;
        cur = next;
    }
    return -1;
}