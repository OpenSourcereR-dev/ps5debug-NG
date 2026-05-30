// SPDX-License-Identifier: GPL-3.0-only

#include "protocol.h"
#include "sdk_shim.h"
#include "net.h"
#include "proc.h"
#include "kern_rw_fast.h"
#include <ps5/kernel.h>

static int      vmmap_offs_initialized = 0;
static uint64_t vmmap_nentries_adj = 0;
static uint64_t vmmap_name_adj     = 0;

static void vmmap_init_offsets(void) {
    uint32_t fw = kernel_get_fw_version();

    int write_adj = 0;
    switch (fw & 0xffff0000u) {
    case 0x6000000u: case 0x6020000u: case 0x6500000u:                  /* 6.00 6.02 6.50 */
    case 0x7000000u: case 0x7010000u:                                   /* 7.00 7.01 */
    case 0x8000000u: case 0x8200000u: case 0x8400000u: case 0x8600000u: /* 8.00 8.20 8.40 8.60 */
    case 0x9000000u: case 0x9050000u: case 0x9200000u:
    case 0x9400000u: case 0x9600000u:                                   /* 9.00 9.05 9.20 9.40 9.60 */
    case 0x10000000u: case 0x10010000u: case 0x10200000u:
    case 0x10400000u: case 0x10600000u:                                 /* 10.00 10.01 10.20 10.40 10.60 */
    case 0x11000000u: case 0x11200000u: case 0x11400000u:
    case 0x11600000u:                                                   /* 11.00 11.20 11.40 11.60 */
    case 0x12000000u: case 0x12020000u: case 0x12200000u:
    case 0x12400000u: case 0x12600000u: case 0x12700000u:               /* 12.00 12.02 12.20 12.40 12.60 12.70 */
    case 0x13000000u: case 0x13200000u:                                 /* 13.00 13.20 */
        write_adj = 1;
        break;
    default:
        write_adj = 0;
        break;
    }

    if (write_adj) {
        vmmap_nentries_adj = 8;
        vmmap_name_adj     = 0xE;
    }
    vmmap_offs_initialized = 1;
}

int sys_proc_vm_map(uint32_t pid, void **out_maps, int *out_count) {
    if (out_maps)  *out_maps = NULL;
    if (out_count) *out_count = 0;

    if ((int32_t)pid <= 0) return 1;

    if (!vmmap_offs_initialized) vmmap_init_offsets();

    intptr_t kproc = kernel_get_proc_fast((pid_t)pid);
    if (!kproc) return 1;

    intptr_t vmspace_kaddr = 0;
    if (kernel_copyout_fast(kproc + 0x200, &vmspace_kaddr, sizeof(vmspace_kaddr)) != 0) return 1;

    uint8_t vmspace_buf[0x350];
    memset(vmspace_buf, 0, sizeof(vmspace_buf));
    if (kernel_copyout_fast(vmspace_kaddr, vmspace_buf, sizeof(vmspace_buf)) != 0) return 1;

    int32_t n_entries = *(int32_t *)(vmspace_buf + 0x1A8 + (uintptr_t)vmmap_nentries_adj);
    if (n_entries <= 0) return 1;

    void *out_buf = malloc((size_t)n_entries * sizeof(struct proc_vm_map_entry));
    if (!out_buf) return 1;

    intptr_t cur_entry = *(intptr_t *)(vmspace_buf + 8);
    uint8_t *out_p   = (uint8_t *)out_buf;
    uint8_t *out_end = out_p + (size_t)n_entries * sizeof(struct proc_vm_map_entry);

    while (cur_entry != 0) {
        uint8_t entry_buf[0x1C0];
        memset(entry_buf, 0, sizeof(entry_buf));
        if (kernel_copyout_fast(cur_entry, entry_buf, sizeof(entry_buf)) != 0) {
            free(out_buf);
            return 1;
        }

        *(uint64_t *)(out_p + 0x20) = *(uint64_t *)(entry_buf + 0x20);
        *(uint64_t *)(out_p + 0x28) = *(uint64_t *)(entry_buf + 0x28);
        *(uint64_t *)(out_p + 0x30) = *(uint64_t *)(entry_buf + 0x58);

        uint8_t prot_byte = entry_buf[0x64] & 0x0F;
        if (prot_byte == 4) prot_byte = 5;
        *(uint16_t *)(out_p + 0x38) = (uint16_t)prot_byte;

        memcpy(out_p, entry_buf + 0x142 + (uintptr_t)vmmap_name_adj, 0x20);

        cur_entry = *(intptr_t *)(entry_buf + 8);
        out_p += sizeof(struct proc_vm_map_entry);
        if (out_p >= out_end) break;
    }

    if (out_maps)  *out_maps  = out_buf;
    if (out_count) *out_count = n_entries;
    return 0;
}

int proc_scan_handle(int fd, struct cmd_packet *packet) {
    struct cmd_proc_scan_packet *sp = (struct cmd_proc_scan_packet *)packet->data;
    if (!sp) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    uint64_t value_length = proc_scan_getSizeOfValueType(sp->valueType);
    if (value_length == 0) value_length = sp->lenData;

    uint8_t *data = (uint8_t *)net_alloc_buffer(sp->lenData);
    if (!data) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    net_send_int32(fd, CMD_SUCCESS);
    net_recv_all(fd, data, sp->lenData, 1);

    void *maps = NULL;
    int count = 0;
    if (sys_proc_vm_map(sp->pid, &maps, &count) != 0) {
        net_send_int32(fd, CMD_ERROR);
        free(data);
        return 1;
    }

    net_send_int32(fd, CMD_SUCCESS);

    const uint8_t *extra = (sp->lenData == value_length) ? NULL : &data[value_length];
    uint8_t *scan_buf = (uint8_t *)net_alloc_buffer(0x4000);

    if (count > 0 && scan_buf) {
        struct proc_vm_map_entry *entry = (struct proc_vm_map_entry *)maps;
        for (int i = 0; i < count; i++,
                                 entry = (struct proc_vm_map_entry *)
                                         ((uint8_t *)entry + sizeof(*entry))) {
            if (!(entry->prot & 1)) continue;

            uint64_t start = entry->start;
            uint64_t section_len = entry->end - start;
            if (!section_len) continue;

            for (uint64_t j = 0; j < section_len; j += value_length) {
                uint64_t off = j & 0x3FFF;
                if (j == 0 || off == 0) {

                    proc_read_mem(sp->pid, start, 0x4000, scan_buf);
                    off = j & 0x3FFF;
                }
                uint64_t cur_addr = start + j;
                if (proc_scan_legacy_compareValues(sp->compareType, sp->valueType,
                                                   value_length, data,
                                                   &scan_buf[off], extra)) {
                    net_send_all(fd, &cur_addr, 8);
                }
            }
        }
    }

    uint64_t endflag = 0xFFFFFFFFFFFFFFFFull;
    net_send_all(fd, &endflag, 8);

    if (scan_buf) free(scan_buf);
    if (maps)     free(maps);
    free(data);
    return 0;
}

int proc_scan_aob_handle(int fd, struct cmd_packet *packet) {
    struct cmd_proc_scan_aob_packet *sp = (struct cmd_proc_scan_aob_packet *)packet->data;
    if (!sp || sp->pattern_length == 0) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    uint32_t plen = sp->pattern_length;

    uint64_t step_unit = proc_scan_getSizeOfValueType(10);
    if (step_unit == 0) step_unit = plen;

    uint8_t *pattern = (uint8_t *)net_alloc_buffer(plen);
    if (!pattern) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }
    uint8_t *mask = (uint8_t *)net_alloc_buffer((uint32_t)step_unit);
    if (!mask) {
        free(pattern);
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    net_send_int32(fd, CMD_SUCCESS);
    net_recv_all(fd, pattern, plen, 1);
    net_recv_all(fd, mask, (int)step_unit, 1);

    uint64_t chunk_size = 0x100000;
    if (chunk_size % step_unit != 0) chunk_size = (chunk_size / step_unit) * step_unit;

    uint8_t *read_buf = (uint8_t *)net_alloc_buffer(chunk_size);
    if (!read_buf) {
        free(pattern);
        free(mask);
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    net_send_int32(fd, CMD_SUCCESS);

    uint64_t address     = sp->address;
    uint64_t remaining   = sp->length;
    uint64_t match_count = 0;
    uint64_t match_addr  = 0;
    uint8_t  stop_flag   = sp->stop_flag;
    uint8_t  max_matches = sp->max_matches;
    int stop_unique = (stop_flag == 1);
    int invalidated = 0;
    int found_done  = 0;

    while (remaining > 0) {
        uint64_t to_read = (chunk_size < remaining) ? chunk_size : remaining;
        memset(read_buf, 0, to_read);
        proc_read_mem(sp->pid, address, to_read, read_buf);

        uint64_t limit = (to_read >= plen) ? to_read - plen : 0;
        for (uint64_t j = 0; j <= limit; j++) {
            if (aob_match(plen, pattern, &read_buf[j], 0, mask)) {
                match_count++;
                if (match_count == max_matches) {
                    match_addr = address + j;
                    if (!stop_unique) { found_done = 1; break; }
                } else if (match_count > max_matches && stop_unique) {
                    match_addr = 0;
                    invalidated = 1;
                    break;
                }
            }
        }
        if (found_done || invalidated) break;
        if (chunk_size >= remaining) break;

        uint64_t advance = chunk_size - plen + 1;
        address  += advance;
        remaining = (remaining > advance) ? remaining - advance : 0;
    }

    net_send_all(fd, &match_addr, 8);
    free(read_buf);
    free(pattern);
    free(mask);
    net_send_int32(fd, CMD_SUCCESS);
    return 0;
}

int proc_scan_aob_multi_handle(int fd, struct cmd_packet *packet) {
    struct cmd_proc_scan_aob_multi_packet *mp =
        (struct cmd_proc_scan_aob_multi_packet *)packet->data;
    if (!mp || mp->patterns_length == 0) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    uint32_t patterns_length = mp->patterns_length;
    uint8_t *blob = (uint8_t *)net_alloc_buffer(patterns_length);
    if (!blob) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    net_send_int32(fd, CMD_SUCCESS);
    net_recv_all(fd, blob, patterns_length, 1);

    uint16_t pat_count = 0;
    uint64_t max_plen  = 1;
    {
        uint32_t cursor = 0;
        while (cursor + 5 <= patterns_length) {
            uint32_t plen;
            memcpy(&plen, blob + cursor + 1, 4);
            cursor += 5 + 2 * plen;
            if (plen > max_plen) max_plen = plen;
            pat_count++;
        }
    }
    uint32_t output_size = (uint32_t)pat_count * 8u;

    uint8_t *read_buf = (uint8_t *)net_alloc_buffer(0x100000);
    void    *output   = read_buf ? net_alloc_buffer(output_size) : NULL;
    uint64_t *match_counts = output ? (uint64_t *)net_alloc_buffer((uint32_t)pat_count * 8u) : NULL;
    if (!read_buf || !output || !match_counts) {
        if (read_buf)    free(read_buf);
        if (output)      free(output);
        if (match_counts) free(match_counts);
        free(blob);
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }
    memset(output, 0, output_size);
    memset(match_counts, 0, (size_t)pat_count * 8u);

    net_send_int32(fd, CMD_SUCCESS);

    uint64_t address    = mp->address;
    uint64_t remaining  = mp->length;
    uint8_t  stop_flag  = mp->stop_flag;
    int      stop_unique = (stop_flag == 1);
    uint16_t found_count = 0;
    uint16_t invalidated_count = 0;

    if (remaining > 0) {
        for (;;) {
            uint64_t to_read = (remaining < 0x100000) ? remaining : 0x100000;
            memset(read_buf, 0, to_read);
            proc_read_mem(mp->pid, address, to_read, read_buf);

            uint32_t cursor = 0;
            int outer_break = 0;
            for (uint16_t pi = 0; pi < pat_count; pi++) {
                uint8_t  target_count = blob[cursor];
                uint32_t plen;
                memcpy(&plen, blob + cursor + 1, 4);
                const uint8_t *pat = blob + cursor + 5;
                const uint8_t *msk = pat + plen;

                uint64_t limit = (to_read >= plen) ? to_read - plen : 0;
                for (uint64_t j = 0; j <= limit; j++) {
                    if (aob_match(plen, pat, &read_buf[j], 0, msk)) {
                        match_counts[pi]++;
                        if (match_counts[pi] == target_count) {
                            uint64_t cur_addr = address + j;
                            memcpy((uint8_t *)output + (size_t)pi * 8u, &cur_addr, 8);
                            found_count++;
                            if (!stop_unique) break;
                        } else if (stop_unique &&
                                   match_counts[pi] == (uint64_t)target_count + 1u) {
                            uint64_t zero = 0;
                            memcpy((uint8_t *)output + (size_t)pi * 8u, &zero, 8);
                            invalidated_count++;
                            break;
                        }
                    }
                }

                cursor += 5 + 2u * plen;

                if (!stop_unique && found_count      >= pat_count) { outer_break = 1; break; }
                if ( stop_unique && invalidated_count >= pat_count) { outer_break = 1; break; }
            }

            if (outer_break) break;
            if (remaining <= 0x100000) break;

            uint64_t advance = (uint64_t)0x100001 - max_plen;
            address  += advance;
            remaining = (remaining > advance) ? remaining - advance : 0;
        }
    }

    net_send_all(fd, output, (int)output_size);
    free(match_counts);
    free(output);
    free(read_buf);
    free(blob);
    net_send_int32(fd, CMD_SUCCESS);
    return 0;
}

int proc_scan_start_handle(int fd, struct cmd_packet *packet) {
    if (!(g_proc_auth_state & 2)) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    struct cmd_proc_scan_start_packet *sp = (struct cmd_proc_scan_start_packet *)packet->data;
    if (!sp) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }
    if (sp->compareType > 12 || sp->lenData == 0) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    int needs_scan_value = ((1u << sp->compareType) & 0x114Fu) != 0;
    int is_between       = (sp->compareType == 4);
    int is_arrbytes      = (sp->valueType == 10);
    if (is_between) needs_scan_value = 1;

    uint64_t value_length = proc_scan_getSizeOfValueType(sp->valueType);
    if (value_length == 0) value_length = sp->lenData;
    if (value_length == 0) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    uint8_t *pattern = NULL;
    if (needs_scan_value && sp->lenData) {
        pattern = (uint8_t *)net_alloc_buffer(sp->lenData);
        if (!pattern) {
            net_send_int32(fd, CMD_DATA_NULL);
            return 1;
        }
    }

    net_send_int32(fd, CMD_SUCCESS);
    if (pattern) {
        net_recv_all(fd, pattern, (int)sp->lenData, 1);
    }

    uint8_t *mask = NULL;
    if (is_arrbytes) {
        mask = (uint8_t *)net_alloc_buffer((uint32_t)value_length);
        if (!mask) {
            if (pattern) free(pattern);
            net_send_int32(fd, CMD_DATA_NULL);
            return 1;
        }
        net_recv_all(fd, mask, (int)value_length, 1);
    }

    uint64_t step = sp->alignment ? sp->alignment : value_length;

    uint64_t chunk_size = 0x100000ULL;
    if (chunk_size % value_length != 0) {
        chunk_size = (chunk_size / value_length) * value_length;
    }

    uint8_t *read_buf   = (uint8_t *)net_alloc_buffer(chunk_size);
    uint8_t *result_buf = (uint8_t *)net_alloc_buffer(0x40000);
    if (!read_buf || !result_buf) {
        if (read_buf)   free(read_buf);
        if (result_buf) free(result_buf);
        if (pattern) free(pattern);
        if (mask)    free(mask);
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    net_send_int32(fd, CMD_SUCCESS);

    uint64_t addr         = sp->address;
    uint64_t remaining    = sp->length;
    uint64_t result_len   = 0;
    uint64_t flush_thresh = 0x3FFE8ULL - value_length;

    const void *prev_for_between = is_between ? (pattern + value_length) : NULL;

    while (remaining > 0) {
        uint64_t to_read = (remaining > chunk_size) ? chunk_size : remaining;
        memset(read_buf, 0, to_read);
        proc_read_mem(sp->pid, addr, to_read, read_buf);

        uint64_t limit = (to_read >= value_length) ? to_read - value_length : 0;
        for (uint64_t j = 0; j <= limit; j += step) {
            if (proc_scan_compareValues(sp->compareType, sp->valueType, value_length,
                                        pattern, &read_buf[j], prev_for_between, mask)) {
                if (result_len > flush_thresh) {
                    *(uint64_t *)result_buf = result_len;
                    net_send_all(fd, result_buf, (int)(result_len + 8));
                    result_len = 0;
                }
                uint32_t offset = (uint32_t)((addr + j) - sp->address);
                memcpy(result_buf + 8 + result_len,         &offset,        4);
                memcpy(result_buf + 8 + result_len + 4,     &read_buf[j],   value_length);
                result_len += 4 + value_length;
            }
        }

        uint64_t advance = to_read + step - value_length;
        if (advance == 0 || advance > to_read) advance = to_read;
        addr += advance;
        remaining = (remaining > advance) ? remaining - advance : 0;
    }

    if (result_len) {
        *(uint64_t *)result_buf = result_len;
        net_send_all(fd, result_buf, (int)(result_len + 8));
    }

    uint64_t sentinel = 0xFFFFFFFFFFFFFFFFULL;
    net_send_all(fd, &sentinel, 8);

    free(read_buf);
    free(result_buf);
    if (pattern) free(pattern);
    if (mask)    free(mask);

    net_send_int32(fd, CMD_SUCCESS);
    return 0;
}

int proc_scan_count_handle(int fd, struct cmd_packet *packet) {
    if (!(g_proc_auth_state & 2)) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    struct cmd_proc_scan_count_packet *cp = (struct cmd_proc_scan_count_packet *)packet->data;
    if (!cp) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }
    if (cp->compareType > 12) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    int needs_value_flag    = g_cmptype_needs_value   [cp->compareType] != 0;
    int needs_extra_flag    = g_cmptype_needs_extra   [cp->compareType] != 0;
    int needs_previous_flag = g_cmptype_needs_previous[cp->compareType] != 0;
    int needs_scan_value    = needs_value_flag || needs_extra_flag;
    int is_arrbytes         = (cp->valueType == 10);

    uint64_t value_length = proc_scan_getSizeOfValueType(cp->valueType);
    if (value_length == 0) value_length = cp->lenData;
    if (value_length == 0 || value_length > 0x1000) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    uint8_t *pattern = NULL;
    uint8_t *mask    = NULL;
    if (needs_scan_value && cp->lenData) {
        pattern = (uint8_t *)net_alloc_buffer(cp->lenData);
        if (!pattern) {
            net_send_int32(fd, CMD_DATA_NULL);
            return 1;
        }
    }
    if (is_arrbytes) {
        mask = (uint8_t *)net_alloc_buffer((uint32_t)value_length);
        if (!mask) {
            if (pattern) free(pattern);
            net_send_int32(fd, CMD_DATA_NULL);
            return 1;
        }
    }

    uint8_t *chunk_buf  = (uint8_t *)net_alloc_buffer(0x40000);
    uint8_t *result_buf = (uint8_t *)net_alloc_buffer(0x40000);
    uint8_t *mem_buf    = (uint8_t *)net_alloc_buffer(0x100000);
    if (!chunk_buf || !result_buf || !mem_buf) {
        if (chunk_buf)  free(chunk_buf);
        if (result_buf) free(result_buf);
        if (mem_buf)    free(mem_buf);
        if (pattern)    free(pattern);
        if (mask)       free(mask);
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    net_send_int32(fd, CMD_SUCCESS);
    if (pattern) net_recv_all(fd, pattern, (int)cp->lenData, 1);
    if (mask)    net_recv_all(fd, mask,    (int)value_length, 1);

    int      includes_prev = needs_previous_flag;
    uint64_t entry_size    = includes_prev ? (4 + value_length) : 4;
    uint64_t flush_thresh  = 0x3FFE8ULL - 2 * value_length;

    for (;;) {
        uint32_t chunk_len = 0;
        net_recv_all(fd, &chunk_len, 4, 1);
        if (chunk_len == 0xFFFFFFFFu) break;
        if (chunk_len == 0) {
            uint64_t per_chunk_sentinel = 0xFFFFFFFFFFFFFFFFULL;
            net_send_all(fd, &per_chunk_sentinel, 8);
            continue;
        }
        if (chunk_len > 0x40000u) break;

        net_recv_all(fd, chunk_buf, (int)chunk_len, 1);

        uint64_t last_entry_addr = 0;
        if (chunk_len >= entry_size) {
            uint32_t last_offset;
            memcpy(&last_offset, chunk_buf + chunk_len - entry_size, 4);
            last_entry_addr = cp->base_address + last_offset;
        }

        uint64_t window_start = 0;
        uint64_t window_end   = 0;
        uint64_t result_len   = 0;

        for (uint64_t off = 0; off + entry_size <= chunk_len; off += entry_size) {
            uint32_t entry_offset;
            memcpy(&entry_offset, chunk_buf + off, 4);
            const uint8_t *prev_value_ptr = includes_prev ? (chunk_buf + off + 4) : NULL;
            uint64_t addr = cp->base_address + entry_offset;

            if (addr >= window_end) {

                uint64_t span = (last_entry_addr >= addr)
                              ? (last_entry_addr + value_length) - addr
                              : value_length;
                uint32_t read_size = (span > 0x100000ULL) ? 0x100000u : (uint32_t)span;

                memset(mem_buf, 0, read_size);
                proc_read_mem(cp->pid, addr, (uint64_t)read_size, mem_buf);
                window_start = addr;
                window_end   = addr + read_size;
            }

            const uint8_t *mem_value_ptr = mem_buf + (addr - window_start);

            if (proc_scan_compareValues(cp->compareType, cp->valueType, value_length,
                                        pattern, mem_value_ptr, prev_value_ptr, mask)) {
                if (result_len > flush_thresh) {
                    *(uint64_t *)result_buf = result_len;
                    net_send_all(fd, result_buf, (int)(result_len + 8));
                    result_len = 0;
                }
                memcpy(result_buf + 8 + result_len,     &entry_offset, 4);
                memcpy(result_buf + 8 + result_len + 4, mem_value_ptr, value_length);
                result_len += 4 + value_length;
            }
        }

        if (result_len) {
            *(uint64_t *)result_buf = result_len;
            net_send_all(fd, result_buf, (int)(result_len + 8));
        }

        uint64_t per_chunk_sentinel = 0xFFFFFFFFFFFFFFFFULL;
        net_send_all(fd, &per_chunk_sentinel, 8);
    }

    free(chunk_buf);
    free(result_buf);
    free(mem_buf);
    if (pattern) free(pattern);
    if (mask)    free(mask);

    net_send_int32(fd, CMD_SUCCESS);
    return 0;
}

int proc_scan_get_handle(int fd, struct cmd_packet *packet) {
    if (!(g_proc_auth_state & 2)) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    struct cmd_proc_scan_get_packet *gp = (struct cmd_proc_scan_get_packet *)packet->data;
    if (!gp) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    uint32_t entries_len = gp->count * 12u;
    uint8_t *entries = (uint8_t *)net_alloc_buffer(entries_len);
    if (!entries) {
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    net_send_int32(fd, CMD_SUCCESS);
    net_recv_all(fd, entries, (int)entries_len, 1);

    uint8_t *buf = (uint8_t *)net_alloc_buffer(0x100000);
    if (!buf) {
        free(entries);
        net_send_int32(fd, CMD_DATA_NULL);
        return 1;
    }

    for (uint32_t i = 0; i < gp->count; i++) {
        uint64_t addr;
        uint32_t length32;
        memcpy(&addr,     entries + 12u * i,        8);
        memcpy(&length32, entries + 12u * i + 8u,   4);
        uint64_t length = length32;
        while (length > 0) {
            uint64_t to_read = (length > 0x100000ULL) ? 0x100000ULL : length;
            memset(buf, 0, to_read);
            proc_read_mem(gp->pid, addr, to_read, buf);
            net_send_all(fd, buf, (int)to_read);
            addr   += to_read;
            length -= to_read;
        }
    }

    uint64_t sentinel = 0xFFFFFFFFFFFFFFFFULL;
    net_send_all(fd, &sentinel, 8);

    free(entries);
    free(buf);
    return 0;
}
