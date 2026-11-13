/*
 * op_probe (Windows) — what Windows knows about the attached OpenPort: which
 * driver it has, and the COM port or device interface that driver offers.
 *
 * The driver decides how the library reaches the cable (src/op_win.c), so
 * this is the first thing to look at when PassThruOpen finds nothing. The
 * macOS and Linux op_probe dumps USB descriptors through libusb instead;
 * Windows hands those only to a device's own driver.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>

#include <stdio.h>
#include <string.h>

#include "op_win_ids.h"

static const char *prop(HDEVINFO set, SP_DEVINFO_DATA *dev, DWORD which,
                        char *buf, DWORD cap, DWORD *len)
{
    DWORD n = 0;
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, which, NULL, (BYTE *)buf, cap, &n)) {
        buf[0] = '\0';
        n = 0;
    }
    if (len != NULL) *len = n;
    return buf;
}

/* Every present device with a Tactrix USB id (0403:cc4b, cc4c, cc4d). */
static int list_devices(void)
{
    HDEVINFO set = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    SP_DEVINFO_DATA dev;
    DWORD i;
    int cables = 0;

    if (set == INVALID_HANDLE_VALUE) return 0;
    dev.cbSize = sizeof dev;
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        char ids[1024], text[256];
        DWORD len;

        prop(set, &dev, SPDRP_HARDWAREID, ids, sizeof ids, &len);
        if (!op_win_has_id(ids, len, "VID_0403&PID_CC4")) continue;

        printf("%s\n", ids);
        printf("  %-12s %s\n", "description", prop(set, &dev, SPDRP_DEVICEDESC, text, sizeof text, NULL));
        printf("  %-12s %s\n", "class", prop(set, &dev, SPDRP_CLASS, text, sizeof text, NULL));
        prop(set, &dev, SPDRP_SERVICE, text, sizeof text, NULL);
        printf("  %-12s %s\n", "driver", text[0] != '\0' ? text : "(none)");
        if (op_win_has_id(ids, len, OP_HWID)) cables++;
        else if (op_win_has_id(ids, len, OP_HWID_STORAGE))
            printf("  USB mass storage: a microSD card is in the cable. Remove it and re-plug.\n");
        else if (op_win_has_id(ids, len, OP_HWID_BOOTLOADER))
            printf("  The cable is in its bootloader. Re-plug it.\n");
    }
    SetupDiDestroyDeviceInfoList(set);
    return cables;
}

/* The cable's interfaces of one class: what the library would open. */
static int list_interfaces(const GUID *iface, const char *label)
{
    HDEVINFO set = SetupDiGetClassDevsA(iface, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i;
    int n = 0;

    if (set == INVALID_HANDLE_VALUE) return 0;
    ifd.cbSize = sizeof ifd;
    for (i = 0; SetupDiEnumDeviceInterfaces(set, NULL, iface, i, &ifd); i++) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A d;
            char room[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 512];
        } detail;
        SP_DEVINFO_DATA dev;
        char ids[1024], name[256];
        DWORD len;

        detail.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        dev.cbSize = sizeof dev;
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, &detail.d, sizeof detail, NULL, &dev))
            continue;
        prop(set, &dev, SPDRP_HARDWAREID, ids, sizeof ids, &len);
        if (!op_win_has_id(detail.d.DevicePath, strlen(detail.d.DevicePath), OP_HWID)
            && !op_win_has_id(ids, len, OP_HWID))
            continue;
        prop(set, &dev, SPDRP_FRIENDLYNAME, name, sizeof name, NULL);
        printf("%s: %s\n  %s\n", label, name, detail.d.DevicePath);
        n++;
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

int main(void)
{
    int cables = list_devices();
    int usable;

    if (cables == 0) {
        printf("No OpenPort 2.0 (USB 0403:cc4d) is attached.\n");
        return 1;
    }
    usable = list_interfaces(&OP_TACTRIX_IFACE, "Tactrix driver interface")
           + list_interfaces(&OP_COMPORT_IFACE, "COM port");
    if (usable == 0) {
        printf("The cable is attached, but its driver offers neither Tactrix's interface\n"
               "nor a COM port, so the library cannot open it. Install Tactrix's driver,\n"
               "or remove the cable's driver in Device Manager so that Windows 10 or later\n"
               "uses its own USB serial driver.\n");
        return 1;
    }
    return 0;
}
