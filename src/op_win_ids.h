/*
 * op_win_ids.h — how Windows names the cable: its USB hardware ids and the
 * device interfaces its two drivers offer. Shared by the transport
 * (src/op_win.c) and op_probe.exe, so both look for the same things.
 *
 * Include after <windows.h> and <initguid.h>, in one file of each program.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OP_WIN_IDS_H
#define OP_WIN_IDS_H

#include <stddef.h>
#include <string.h>

#define OP_HWID            "VID_0403&PID_CC4D"
/* With a microSD card inserted the cable enumerates as storage; in its
 * bootloader it is cc4b (PROTOCOL.md section 12). Neither can be driven. */
#define OP_HWID_STORAGE    "VID_0403&PID_CC4C"
#define OP_HWID_BOOTLOADER "VID_0403&PID_CC4B"

/* The device interface Tactrix's driver registers for the cable. */
DEFINE_GUID(OP_TACTRIX_IFACE, 0x6d1781b7, 0xc987, 0x4f6c,
            0x8d, 0x4f, 0x1e, 0xfc, 0x09, 0x8b, 0xea, 0x67);
/* GUID_DEVINTERFACE_COMPORT, spelled out so no DDK header is needed. */
DEFINE_GUID(OP_COMPORT_IFACE, 0x86e0d1e0, 0x8089, 0x11d0,
            0x9c, 0xe4, 0x08, 0x00, 0x3e, 0x30, 0x1f, 0x73);

/* Case-insensitive search of a REG_MULTI_SZ, or a plain string, for id. */
static inline int op_win_has_id(const char *multi, size_t len, const char *id)
{
    size_t n = strlen(id), i;
    if (len < n) return 0;
    for (i = 0; i + n <= len; i++)
        if (_strnicmp(multi + i, id, n) == 0) return 1;
    return 0;
}

#endif /* OP_WIN_IDS_H */
