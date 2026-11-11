/*
 * op_win.c — the Windows transport: a device handle read and written with
 * overlapped I/O.
 *
 * Windows gives the cable one of two drivers, and this file opens either:
 *
 *  1. Tactrix's own driver, when its installer has been run. The cable is a
 *     "Tactrix Openport 2.0 J2534 Vehicle Interface" and offers the device
 *     interface OP_TACTRIX_IFACE; ReadFile and WriteFile on that interface
 *     are the bulk pipes. This is the handle Tactrix's DLL uses
 *     (tools/ab-official/iface.reg has the registry shape it looks for).
 *
 *  2. The in-box CDC-ACM driver, usbser.sys, which Windows 10 and later bind
 *     to a cable that has no vendor driver. The cable is then a COM port,
 *     found here by its USB id, not by its number.
 *
 * OPENPORT_DEVICE names a COM port ("COM7") or a device path instead; a
 * simulator behind a COM port is opened the same way.
 *
 * One read is kept posted across calls: a read that outlives its caller's
 * timeout stays pending and the next call collects it, so no data is lost to
 * a cancelled read and the pipe is never left without a reader for longer
 * than the caller takes to come back.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "op_printf.h"
#include "op_transport.h"
#include "op_log.h"

#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <mmsystem.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

/* A multiple of the 64-byte packet, so a bulk read ends on a short packet
 * and never overflows. */
#define WIN_RX_LEN 4096u

typedef struct {
    HANDLE     h;
    int        is_comm;       /* a serial port: comm timeouts apply */
    OVERLAPPED rd, wr;
    int        rd_pending;    /* a ReadFile into rbuf has not completed */
    uint8_t    rbuf[WIN_RX_LEN];
    size_t     rpos, rlen;    /* bytes of rbuf not yet handed out */
} op_win;

static _Thread_local char g_detail[320];

OP_PRINTF(1, 2)
static void set_detail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_detail, sizeof g_detail, fmt, ap);
    va_end(ap);
}

const char *op_transport_last_detail(void)
{
    return g_detail[0] != '\0' ? g_detail : NULL;
}

/* An error from a handle that was open and working. Windows has no single
 * code for an unplugged device: which one arrives depends on the driver. */
static op_status from_io_error(DWORD e)
{
    switch (e) {
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_DEVICE_REMOVED:
    case ERROR_NO_SUCH_DEVICE:
    case ERROR_GEN_FAILURE:
    case ERROR_BAD_COMMAND:
    case ERROR_BAD_UNIT:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_INVALID_HANDLE:
    case ERROR_ACCESS_DENIED:
    case ERROR_OPERATION_ABORTED:
    case ERROR_BROKEN_PIPE:       /* Wine, when a pty's other end closes */
        return OP_ERR_NO_DEVICE;
    default:
        return OP_ERR_IO;
    }
}

static op_status win_write(op_transport *t, const uint8_t *buf, size_t len,
                           unsigned timeout_ms)
{
    op_win *w;
    size_t sent = 0;

    if (t == NULL || t->impl == NULL || (buf == NULL && len > 0))
        return OP_ERR_PARAM;
    w = (op_win *)t->impl;
    if (timeout_ms == 0) timeout_ms = 1;

    op_log_bytes("TX", buf, len);

    /* One WriteFile carries the whole command: the cable rejects a payload
     * that arrives late (PROTOCOL.md section 1). The loop only runs again
     * if the driver takes less than it was given. */
    while (sent < len) {
        DWORD n = 0, e;

        ResetEvent(w->wr.hEvent);
        if (!WriteFile(w->h, buf + sent, (DWORD)(len - sent), &n, &w->wr)) {
            e = GetLastError();
            if (e != ERROR_IO_PENDING) {
                op_logf("write: error %lu", (unsigned long)e);
                return from_io_error(e);
            }
            if (WaitForSingleObject(w->wr.hEvent, timeout_ms) != WAIT_OBJECT_0) {
                /* Cancelled, then reaped: the OVERLAPPED must be idle before
                 * it is used again, and the write may have finished first. */
                CancelIoEx(w->h, &w->wr);
                if (!GetOverlappedResult(w->h, &w->wr, &n, TRUE)) n = 0;
                sent += n;
                if (sent >= len) break;
                return sent == 0 ? OP_ERR_STALLED : OP_ERR_TIMEOUT;
            }
            if (!GetOverlappedResult(w->h, &w->wr, &n, FALSE)) {
                e = GetLastError();
                op_logf("write: error %lu", (unsigned long)e);
                return from_io_error(e);
            }
        }
        if (n == 0) return OP_ERR_IO;
        sent += n;
    }
    return OP_OK;
}

static op_status win_read(op_transport *t, uint8_t *buf, size_t cap,
                          size_t *got, unsigned timeout_ms)
{
    op_win *w;
    DWORD n = 0, e;
    size_t take;

    if (got != NULL) *got = 0;
    if (t == NULL || t->impl == NULL || buf == NULL || got == NULL || cap == 0)
        return OP_ERR_PARAM;
    w = (op_win *)t->impl;

    if (w->rpos == w->rlen) {
        int waited = 0;

        if (!w->rd_pending) {
            ResetEvent(w->rd.hEvent);
            if (ReadFile(w->h, w->rbuf, WIN_RX_LEN, &n, &w->rd)) {
                /* completed at once; n is set */
            } else if ((e = GetLastError()) == ERROR_IO_PENDING) {
                w->rd_pending = 1;
            } else {
                op_logf("read: error %lu", (unsigned long)e);
                return from_io_error(e);
            }
        }
        if (w->rd_pending) {
            if (WaitForSingleObject(w->rd.hEvent, timeout_ms) != WAIT_OBJECT_0)
                return OP_ERR_TIMEOUT;          /* the read stays posted */
            w->rd_pending = 0;
            waited = 1;
            if (!GetOverlappedResult(w->h, &w->rd, &n, FALSE)) {
                e = GetLastError();
                op_logf("read: error %lu", (unsigned long)e);
                return from_io_error(e);
            }
        }
        if (n == 0) {
            /* A serial port's read timed out with nothing, or the pipe sent
             * a zero-length packet. Neither is data. A driver that answers
             * every read with nothing at once must not spin the caller. */
            if (!waited) Sleep(timeout_ms < 10 ? timeout_ms : 10);
            return OP_ERR_TIMEOUT;
        }
        w->rpos = 0;
        w->rlen = n;
    }

    take = w->rlen - w->rpos;
    if (take > cap) take = cap;
    memcpy(buf, w->rbuf + w->rpos, take);
    w->rpos += take;
    *got = take;
    op_log_bytes("RX", buf, take);
    return OP_OK;
}

static void win_close(op_transport *t)
{
    op_win *w;
    DWORD n;

    if (t == NULL || t->impl == NULL) return;
    w = (op_win *)t->impl;
    if (w->h != INVALID_HANDLE_VALUE) {
        CancelIoEx(w->h, NULL);
        /* The posted read writes into rbuf until it has been reaped. */
        if (w->rd_pending) (void)GetOverlappedResult(w->h, &w->rd, &n, TRUE);
        CloseHandle(w->h);
    }
    if (w->rd.hEvent != NULL) CloseHandle(w->rd.hEvent);
    if (w->wr.hEvent != NULL) CloseHandle(w->wr.hEvent);
    free(w);
    timeEndPeriod(1);
    t->impl = NULL;
    t->ops  = NULL;
}

static const op_transport_ops g_win_ops = { win_write, win_read, win_close };

/* Wine's ntdll exports its version; Windows' has no such symbol. */
static int under_wine(void)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    return nt != NULL && GetProcAddress(nt, "wine_get_version") != NULL;
}

/* Raw 8N1 with no flow control, and reads that return as soon as any byte
 * is there. Fails on a handle that is not a serial port, which is how the
 * two kinds of handle are told apart. */
static int comm_setup(HANDLE h)
{
    DCB dcb;
    COMMTIMEOUTS ct;

    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) return 0;

    /* The rate means nothing to a CDC-ACM device, but must be valid. The
     * protocol is binary both ways: no XON/XOFF, no hardware handshake, no
     * character replaced or dropped. DTR and RTS are raised, as opening the
     * serial device raises them on macOS and Linux. */
    dcb.BaudRate        = CBR_115200;
    dcb.ByteSize        = 8;
    dcb.Parity          = NOPARITY;
    dcb.StopBits        = ONESTOPBIT;
    dcb.fBinary         = TRUE;
    dcb.fParity         = FALSE;
    dcb.fOutxCtsFlow    = FALSE;
    dcb.fOutxDsrFlow    = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl     = DTR_CONTROL_ENABLE;
    dcb.fRtsControl     = RTS_CONTROL_ENABLE;
    dcb.fOutX           = FALSE;
    dcb.fInX            = FALSE;
    dcb.fErrorChar      = FALSE;
    dcb.fNull           = FALSE;
    dcb.fAbortOnError   = FALSE;
    if (!SetCommState(h, &dcb))
        op_logf("SetCommState: error %lu", (unsigned long)GetLastError());

    /* This combination is the documented one for "return what is there; if
     * nothing is, wait for the first byte, at most the constant". A read that
     * waits out the constant completes with zero bytes and is posted again.
     * Writes carry no timeout of their own: win_write cancels them.
     *
     * Wine (9.0) does not know the combination and waits for the whole
     * buffer. There the interval alone does the same job: an overlapped read
     * completes when the first byte arrives. On Windows the interval alone
     * would complete every read at once, empty or not. */
    memset(&ct, 0, sizeof ct);
    ct.ReadIntervalTimeout = MAXDWORD;
    if (!under_wine()) {
        ct.ReadTotalTimeoutMultiplier = MAXDWORD;
        ct.ReadTotalTimeoutConstant   = 1000;
    }
    if (!SetCommTimeouts(h, &ct))
        op_logf("SetCommTimeouts: error %lu", (unsigned long)GetLastError());

    (void)SetupComm(h, 65536, 4096);
    (void)PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return 1;
}

static op_status open_path(op_transport *out, const char *path, const char *what)
{
    op_win *w;
    HANDLE h;

    /* No sharing: a second program reading the cable would take half its
     * replies. */
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        op_logf("open %s: error %lu", path, (unsigned long)e);
        switch (e) {
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_BUSY:
            set_detail("%s is in use. Another program may have the cable open.", what);
            return OP_ERR_ACCESS;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            set_detail("%s does not exist.", what);
            return OP_ERR_NO_DEVICE;
        default:
            set_detail("%s could not be opened (Windows error %lu).", what, (unsigned long)e);
            return OP_ERR_IO;
        }
    }

    w = (op_win *)calloc(1, sizeof *w);
    if (w != NULL) {
        w->h = h;
        w->rd.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        w->wr.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    }
    if (w == NULL || w->rd.hEvent == NULL || w->wr.hEvent == NULL) {
        if (w != NULL && w->rd.hEvent != NULL) CloseHandle(w->rd.hEvent);
        if (w != NULL && w->wr.hEvent != NULL) CloseHandle(w->wr.hEvent);
        free(w);
        CloseHandle(h);
        set_detail("out of memory");
        return OP_ERR_IO;
    }
    w->is_comm = comm_setup(h);

    /* The system timer ticks every 15.6 ms unless asked otherwise, and every
     * timed wait in the library would be rounded up to it. */
    timeBeginPeriod(1);

    out->ops  = &g_win_ops;
    out->impl = w;
    op_logf("open: %s (%s)", path, w->is_comm ? "serial port" : "device interface");
    return OP_OK;
}

op_status op_serial_open(op_transport *out, const char *path)
{
    char full[300];

    if (out == NULL || path == NULL) return OP_ERR_PARAM;
    out->ops = NULL;
    out->impl = NULL;
    /* "COM7" as typed; CreateFile needs the device namespace for COM10 up. */
    if (path[0] != '\\') {
        snprintf(full, sizeof full, "\\\\.\\%s", path);
        return open_path(out, full, path);
    }
    return open_path(out, path, path);
}

/* Case-insensitive search of a REG_MULTI_SZ (or a plain string) for id. */
static int multi_sz_has(const char *multi, size_t len, const char *id)
{
    size_t n = strlen(id), i;
    if (len < n) return 0;
    for (i = 0; i + n <= len; i++)
        if (_strnicmp(multi + i, id, n) == 0) return 1;
    return 0;
}

static int hwid_has(HDEVINFO set, SP_DEVINFO_DATA *dev, const char *id)
{
    char ids[1024];
    DWORD type = 0, len = 0;
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_HARDWAREID, &type,
                                           (BYTE *)ids, sizeof ids, &len))
        return 0;
    return multi_sz_has(ids, len < sizeof ids ? len : sizeof ids, id);
}

/* The path of the first present interface of class `iface` that belongs to
 * the cable. A Tactrix interface names the USB id in its path; a COM port's
 * path does not have to, so the device's hardware id is asked as well. */
static int find_interface(const GUID *iface, char *path, size_t cap)
{
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i;
    int found = 0;

    set = SetupDiGetClassDevsA(iface, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;

    ifd.cbSize = sizeof ifd;
    for (i = 0; !found && SetupDiEnumDeviceInterfaces(set, NULL, iface, i, &ifd); i++) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A d;
            char room[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 512];
        } detail;
        SP_DEVINFO_DATA dev;

        detail.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        dev.cbSize = sizeof dev;
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, &detail.d, sizeof detail, NULL, &dev))
            continue;
        if (multi_sz_has(detail.d.DevicePath, strlen(detail.d.DevicePath), OP_HWID)
            || hwid_has(set, &dev, OP_HWID)) {
            snprintf(path, cap, "%s", detail.d.DevicePath);
            found = 1;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

/* Whether any present device carries this hardware id; its driver's service
 * name into service, when it has one. */
static int device_present(const char *id, char *service, size_t cap)
{
    HDEVINFO set;
    SP_DEVINFO_DATA dev;
    DWORD i;
    int found = 0;

    if (service != NULL && cap > 0) service[0] = '\0';
    set = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return 0;
    dev.cbSize = sizeof dev;
    for (i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        if (!hwid_has(set, &dev, id)) continue;
        found = 1;
        if (service != NULL && cap > 0
            && !SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_SERVICE, NULL,
                                                  (BYTE *)service, (DWORD)cap, NULL))
            service[0] = '\0';
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

op_status op_serial_find(uint16_t vid, uint16_t pid, char *path, size_t cap)
{
    (void)vid; (void)pid;
    if (path == NULL || cap == 0) return OP_ERR_PARAM;
    return find_interface(&OP_COMPORT_IFACE, path, cap) ? OP_OK : OP_ERR_NO_DEVICE;
}

/*
 * Transport selection. OPENPORT_DEVICE names a COM port or a device path.
 * Unset, the cable's Tactrix interface is tried first, then its COM port:
 * a cable has one driver or the other, never both.
 */
op_status op_transport_open(op_transport *out)
{
    const char *dev = getenv("OPENPORT_DEVICE");
    char path[600], service[64];

    if (out == NULL) return OP_ERR_PARAM;
    out->ops = NULL;
    out->impl = NULL;
    g_detail[0] = '\0';

    if (dev != NULL && dev[0] != '\0' && strcmp(dev, "usb") != 0) {
        op_logf("OPENPORT_DEVICE set: using %s", dev);
        return op_serial_open(out, dev);
    }
    if (find_interface(&OP_TACTRIX_IFACE, path, sizeof path))
        return open_path(out, path, "The OpenPort");
    if (find_interface(&OP_COMPORT_IFACE, path, sizeof path))
        return open_path(out, path, "The OpenPort's COM port");

    if (device_present(OP_HWID, service, sizeof service)) {
        set_detail("The OpenPort is attached but its driver (%s) gives no access. "
                   "Use Tactrix's driver or the Windows USB serial driver.",
                   service[0] != '\0' ? service : "none installed");
        return OP_ERR_NO_DEVICE;
    }
    if (device_present(OP_HWID_STORAGE, NULL, 0)) {
        set_detail("The OpenPort enumerated as USB mass storage. Remove the "
                   "microSD card and re-plug the cable.");
        return OP_ERR_MASS_STORAGE;
    }
    if (device_present(OP_HWID_BOOTLOADER, NULL, 0)) {
        set_detail("The OpenPort is in its bootloader (0403:cc4b). Re-plug the cable.");
        return OP_ERR_NO_DEVICE;
    }
    set_detail("No OpenPort (0403:cc4d) found.");
    return OP_ERR_NO_DEVICE;
}
