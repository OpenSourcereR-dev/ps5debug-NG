// SPDX-License-Identifier: GPL-3.0-only

#include "sdk_shim.h"
#include "protocol.h"
#include "proc.h"
#include "kdbg.h"

extern int      kernel_mprotect(pid_t pid, intptr_t addr, uint64_t len, int prot);
extern int      proc_remote_alloc(uint32_t pid, uint64_t *out_addr, uint64_t length, uint64_t flags);
extern int      proc_remote_free (uint32_t pid, uint64_t addr, uint64_t length);
extern int      proc_remote_call (int pid, struct sys_proc_call_args *args);
extern long     sys_proc_elf     (unsigned long pid, unsigned long elf, unsigned long length);
extern long     sys_proc_elf_rpc (unsigned long pid, unsigned long elf, unsigned long length, uint64_t *out_entry);
extern int      sys_proc_vm_map  (uint32_t pid, void **out_maps, int *out_count);

int sys_proc_cmd(uint64_t pid, uint64_t cmd, void *data) {
    if (!data) return 1;

    switch (cmd) {

    case SYS_PROC_VM_MAP: {
        struct sys_proc_vm_map_args *args = (struct sys_proc_vm_map_args *)data;
        void *maps = NULL;
        int   count = 0;

        int rc = sys_proc_vm_map((uint32_t)pid, &maps, &count);
        if (rc != 0) {
            sceKernelUsleep(10000);
            for (int retries = 21; retries > 0; retries--) {
                if (sys_proc_vm_map((uint32_t)pid, &maps, &count) == 0) { rc = 0; break; }
                sceKernelUsleep(10000);
            }
            if (rc != 0) return 1;
        }

        if (!args->maps) {

            args->num = (uint64_t)(count + 1);
            if (maps) free(maps);
        } else {

            uint64_t to_copy = args->num < (uint64_t)count ? args->num : (uint64_t)count;
            if (maps && to_copy > 0) {
                memcpy(args->maps, maps,
                       (size_t)to_copy * sizeof(struct proc_vm_map_entry));
            }
            args->num = to_copy;
            if (maps) free(maps);
        }
        return 0;
    }

    case SYS_PROC_ALLOC: {
        struct sys_proc_alloc_args *args = (struct sys_proc_alloc_args *)data;
        uint64_t out_addr = 0;

        if (proc_remote_alloc((uint32_t)pid, &out_addr, args->length, 0x4000) != 0) return 1;
        args->address = out_addr;
        return 0;
    }

    case SYS_PROC_ALLOC_HINTED: {
        struct sys_proc_alloc_args *args = (struct sys_proc_alloc_args *)data;

        uint64_t out_addr = args->address;
        if (proc_remote_alloc((uint32_t)pid, &out_addr, args->length, args->address) != 0) {

            args->address = out_addr;
            return 1;
        }
        args->address = out_addr;
        return 0;
    }

    case SYS_PROC_FREE: {
        struct sys_proc_free_args *args = (struct sys_proc_free_args *)data;
        return proc_remote_free((uint32_t)pid, args->address, args->length);
    }

    case SYS_PROC_PROTECT: {
        struct sys_proc_protect_args *args = (struct sys_proc_protect_args *)data;
        return kernel_mprotect((pid_t)pid, (intptr_t)args->address,
                               args->length, (int)args->prot);
    }

    case SYS_PROC_CALL:
        return proc_remote_call((int)pid, (struct sys_proc_call_args *)data);

    case SYS_PROC_ELF: {
        struct sys_proc_elf_args *args = (struct sys_proc_elf_args *)data;

        long elf_rc = sys_proc_elf((unsigned long)pid,
                                   (unsigned long)(uintptr_t)args->elf,
                                   (unsigned long)args->length);
        return elf_rc != 0 ? 1 : 0;
    }

    case SYS_PROC_ELF_RPC: {
        struct sys_proc_elf_rpc_args *args = (struct sys_proc_elf_rpc_args *)data;
        uint64_t entry = 0;
        long elf_rc = sys_proc_elf_rpc((unsigned long)pid,
                                       (unsigned long)(uintptr_t)args->elf,
                                       (unsigned long)args->length,
                                       &entry);
        args->entry = entry;
        return elf_rc != 0 ? 1 : 0;
    }

    case SYS_PROC_INSTALL:
    case SYS_PROC_INFO:
    case SYS_PROC_THRINFO:
    default:
        return 1;
    }
}