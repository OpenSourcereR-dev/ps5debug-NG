// SPDX-License-Identifier: GPL-3.0-only

#include <ps5/kernel.h>
#include "proc_field_offsets.h"

int proc_get_field_offsets(struct proc_field_offsets *out)
{
    static int cached = 0;
    static struct proc_field_offsets c;

    if (!cached) {
        uint32_t fw = kernel_get_fw_version() & 0xffff0000u;

        c.name = fw >= 0x12000000u ? 0x5E4u :
                 fw >= 0x10000000u ? 0x5DCu :
                 fw >= 0x7000000u  ? 0x5D4u :
                 fw >= 0x6000000u  ? 0x5C4u : 0x59Cu;

        c.path = fw >= 0x12000000u ? 0x604u :
		         fw >= 0x10000000u ? 0x5FCu :
                 fw >= 0x7000000u  ? 0x5F4u :
                 fw >= 0x6000000u  ? 0x5E4u : 0x5BCu;

        c.titleid = fw >= 0x8000000u ? 0x470u :
                    fw >= 0x7000000u ? 0x49Au :
                    fw >= 0x6000000u ? 0x498u : 0x470u;

        c.contentid = fw >= 0x12000000u ? 0x504u :
                      fw >= 0x8000000u ? 0x4F4u :
                      fw >= 0x7000000u ? 0x4FCu :
                      fw >= 0x6000000u ? 0x4ECu : 0x4C4u;

        c.known = 1;
        cached = 1;
    }

    *out = c;
    return 0;
}
