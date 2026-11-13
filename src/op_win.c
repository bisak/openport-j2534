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
 * Receiving: the cable sends each record as its own short packet and holds
 * about 1.1 KB of them, some 17 ms of a dense stream, for a host that is not
 * asking; past that it discards new CAN frames and marks nothing (measured
 * 2026-10-23, PROTOCOL.md section 1). Tactrix's driver hands each ReadFile to
 * the bulk pipe, and Tactrix's DLL keeps one posted. Here WIN_RX_READS stay
 * posted: win_read collects the oldest and posts it again at once, so the
 * driver holds the rest while the process is busy or frozen. win_read runs
 * only on the device's reader thread (op_device.c), which lives as long as the
 * handle, so its reads are never cancelled by a thread that exits, and there
 * is no second thread to wake. A COM port gets one read: usbser.sys reads the
 * cable itself and buffers, as the serial driver does on macOS.
 *
 * Sending: win_write returns once the whole command is with the driver,
 * carrying it on past the caller's timeout as long as OP_WRITE_GRACE_MS
 * allows, so it is never left half sent. It does not return with the write
 * still pending: Windows cancels a thread's I/O when the thread exits, and
 * an application may write from a thread that exits at once (a test of 300
 * such threads lost half their frames that way, 2026-11-13).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "op_printf.h"
#include "op_transport.h"
#include "op_log.h"
#include "op_env.h"

#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <mmsystem.h>

#include "op_win_ids.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* After a command goes out, the reader watches for its reply this long
 * before it sleeps. The cable answers a command of its own in about 1 ms;
 * a thread woken from sleep on this PC costs about 0.2 ms more, and the
 * watching keeps a core awake so every wake in the exchange is fast. A
 * configuration command then takes 0.87 ms, as with Tactrix's DLL, where it
 * took 1.15 ms (measured 2026-11-12). It costs at most this much CPU a
 * command, and nothing while no command waits. */
#define WIN_REPLY_WATCH_US 1500u

/* 256 packets is at least 60 ms of a dense stream, on top of the cable's own
 * 17 ms. A read's buffer is a multiple of the 64-byte packet, so a bulk read
 * ends on a short packet and never overflows. */
#define WIN_RX_READS 256u
#define WIN_RX_LEN   256u

typedef struct {
    OVERLAPPED ov;
    int        posted;          /* with the driver, not yet collected */
    uint8_t    buf[WIN_RX_LEN];
} rx_slot;

typedef struct {
    HANDLE     h;
    int        is_comm;         /* a serial port: comm timeouts apply */

    OVERLAPPED wr;
    /* now_us() when the last command went out: written by the caller,
     * read by the reader thread. */
    atomic_ullong wr_issued_us;

    /* The reader thread's alone. Reads complete in the order they were
     * posted, slot 0 first and round, so head is always the oldest. */
    rx_slot   *rx;
    unsigned   nreads, head;
    size_t     head_len, head_pos;  /* head's bytes in hand, and handed out */
    int        started;
    /* The handle can no longer be trusted: a read failed, or a write could
     * not be cancelled. Set by either thread, so atomic. */
    atomic_int gone;
    int        wr_stuck;        /* w->wr is still with the driver */
    /* The most reads found complete when the reader came for the oldest: all
     * of them means the pipe went unpolled and the cable may have discarded
     * frames. Close logs it. */
    unsigned   waiting_most;
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
    case ERROR_OPERATION_ABORTED: /* both drivers, cable unplugged (measured) */
    case ERROR_BROKEN_PIPE:       /* Wine, when a pty's other end closes */
        return OP_ERR_NO_DEVICE;
    default:
        return OP_ERR_IO;
    }
}

/* Microseconds on the performance counter: GetTickCount64 moves in steps
 * of the 15.6 ms system tick, which timeBeginPeriod does not shorten. */
static ULONGLONG now_us(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (ULONGLONG)c.QuadPart / (ULONGLONG)f.QuadPart * 1000000u
         + (ULONGLONG)c.QuadPart % (ULONGLONG)f.QuadPart * 1000000u / (ULONGLONG)f.QuadPart;
}

static ULONGLONG now_ms(void)
{
    return now_us() / 1000u;
}

/* WaitForSingleObject until a deadline on now_ms(), which may be further off
 * than a DWORD reaches; a single wait could also come out as INFINITE. */
static DWORD wait_until(HANDLE ev, ULONGLONG deadline)
{
    for (;;) {
        ULONGLONG now = now_ms(), left = now < deadline ? deadline - now : 0;
        DWORD r = WaitForSingleObject(ev, left > 0x7FFFFFFFu ? 0x7FFFFFFFu : (DWORD)left);
        if (r != WAIT_TIMEOUT || left <= 0x7FFFFFFFu) return r;
    }
}

static op_status win_write(op_transport *t, const uint8_t *buf, size_t len,
                           unsigned timeout_ms)
{
    op_win *w;
    size_t done = 0;
    ULONGLONG deadline;

    if (t == NULL || t->impl == NULL || (buf == NULL && len > 0) || len > MAXDWORD)
        return OP_ERR_PARAM;
    w = (op_win *)t->impl;
    if (atomic_load(&w->gone)) return OP_ERR_NO_DEVICE;
    if (len == 0) return OP_OK;

    op_log_bytes("TX", buf, len);
    deadline = now_ms() + (timeout_ms ? timeout_ms : 1u) + OP_WRITE_GRACE_MS;
    atomic_store_explicit(&w->wr_issued_us, now_us(), memory_order_relaxed);
    /* One WriteFile carries the whole command: the cable rejects a payload
     * that arrives late (PROTOCOL.md section 1). */
    while (done < len) {
        HANDLE ev = w->wr.hEvent;
        DWORD n = 0, e;

        memset(&w->wr, 0, sizeof w->wr);
        w->wr.hEvent = ev;
        ResetEvent(ev);
        if (!WriteFile(w->h, buf + done, (DWORD)(len - done), NULL, &w->wr)
            && (e = GetLastError()) != ERROR_IO_PENDING) {
            op_logf("write: error %lu", (unsigned long)e);
            return from_io_error(e);
        }
        if (wait_until(ev, deadline) != WAIT_OBJECT_0) {
            /* Cancelled, then reaped: the write may have finished first. A
             * driver that does not complete the cancel within a second keeps
             * w->wr and the caller's buffer, so the handle is given up. */
            CancelIoEx(w->h, &w->wr);
            if (WaitForSingleObject(ev, 1000) != WAIT_OBJECT_0) {
                op_logf("write: the driver did not give the write back; the cable is given up");
                w->wr_stuck = 1;
                atomic_store(&w->gone, 1);
                return OP_ERR_NO_DEVICE;
            }
            if (GetOverlappedResult(w->h, &w->wr, &n, FALSE) && done + n >= len) return OP_OK;
            op_logf("write: the cable took %zu of %zu bytes before the time ran out",
                    done + (size_t)w->wr.InternalHigh, len);
            return OP_ERR_STALLED;
        }
        if (!GetOverlappedResult(w->h, &w->wr, &n, FALSE)) {
            e = GetLastError();
            op_logf("write: error %lu", (unsigned long)e);
            return from_io_error(e);
        }
        if (n == 0) return OP_ERR_IO;
        done += n;
    }
    return OP_OK;
}

/* Posts one read. */
static op_status rx_post(op_win *w, rx_slot *s)
{
    HANDLE ev = s->ov.hEvent;
    DWORD e;

    memset(&s->ov, 0, sizeof s->ov);
    s->ov.hEvent = ev;
    ResetEvent(ev);
    /* Done at once or pending, the result is collected the same way. */
    if (ReadFile(w->h, s->buf, WIN_RX_LEN, NULL, &s->ov)
        || (e = GetLastError()) == ERROR_IO_PENDING) {
        s->posted = 1;
        return OP_OK;
    }
    op_logf("read: error %lu", (unsigned long)e);
    return from_io_error(e);
}

/* Any failure to post or collect a read ends the session: the reads complete
 * in the order they were posted, and a slot posted out of turn would hand
 * the cable's bytes over out of order. */
static op_status rx_failed(op_win *w, op_status st)
{
    atomic_store(&w->gone, 1);
    return st == OP_ERR_NO_DEVICE ? st : OP_ERR_NO_DEVICE;
}

static op_status win_read(op_transport *t, uint8_t *buf, size_t cap,
                          size_t *got, unsigned timeout_ms)
{
    op_win *w;
    rx_slot *s;
    size_t take;
    unsigned i;

    if (got != NULL) *got = 0;
    if (t == NULL || t->impl == NULL || buf == NULL || got == NULL || cap == 0)
        return OP_ERR_PARAM;
    w = (op_win *)t->impl;
    if (atomic_load(&w->gone)) return OP_ERR_NO_DEVICE;

    if (!w->started) {
        /* This thread only moves the cable's bytes into the channel queues,
         * and must run whenever they arrive. */
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        w->started = 1;
        for (i = 0; i < w->nreads; i++) {
            op_status st = rx_post(w, &w->rx[i]);
            if (st != OP_OK) return rx_failed(w, st);
        }
    }

    s = &w->rx[w->head];
    if (w->head_len == 0) {
        DWORD n = 0;
        while (!HasOverlappedIoCompleted(&s->ov)
               && now_us() - atomic_load_explicit(&w->wr_issued_us, memory_order_relaxed)
                  < WIN_REPLY_WATCH_US)
            SwitchToThread();               /* lets anything else ready run */
        if (WaitForSingleObject(s->ov.hEvent, timeout_ms) != WAIT_OBJECT_0)
            return OP_ERR_TIMEOUT;                  /* it stays posted */
        for (i = 1; i < w->nreads; i++) {
            const rx_slot *later = &w->rx[(w->head + i) % w->nreads];
            if (!later->posted || !HasOverlappedIoCompleted(&later->ov)) break;
        }
        if (i > w->waiting_most) w->waiting_most = i;
        s->posted = 0;
        if (!GetOverlappedResult(w->h, &s->ov, &n, FALSE)) {
            DWORD e = GetLastError();
            op_logf("read: error %lu", (unsigned long)e);
            return rx_failed(w, from_io_error(e));
        }
        if (n == 0) {
            /* A serial port's read waited out its timeout. */
            op_status st = rx_post(w, s);
            if (st != OP_OK) return rx_failed(w, st);
            w->head = (w->head + 1) % w->nreads;
            return OP_ERR_TIMEOUT;
        }
        w->head_len = n;
        w->head_pos = 0;
    }

    take = w->head_len - w->head_pos;
    if (take > cap) take = cap;
    memcpy(buf, s->buf + w->head_pos, take);
    w->head_pos += take;
    if (w->head_pos == w->head_len) {
        w->head_len = 0;
        if (rx_post(w, s) != OP_OK) atomic_store(&w->gone, 1);   /* this read's bytes still go out */
        w->head = (w->head + 1) % w->nreads;
    }
    *got = take;
    op_log_bytes("RX", buf, take);
    return OP_OK;
}

/* Frees what open_path allocated, once nothing the driver holds points into
 * it. The reader thread has exited by now, which cancelled its reads. */
static void win_free(op_win *w)
{
    unsigned i;
    int idle = 1;
    ULONGLONG until;

    if (w->h != INVALID_HANDLE_VALUE) CancelIoEx(w->h, NULL);
    until = now_ms() + 1000;                /* for all of them together */
    for (i = 0; w->rx != NULL && i < w->nreads; i++) {
        ULONGLONG now = now_ms();
        DWORD n;
        if (!w->rx[i].posted) continue;
        if (WaitForSingleObject(w->rx[i].ov.hEvent, now < until ? (DWORD)(until - now) : 0)
            == WAIT_OBJECT_0)
            (void)GetOverlappedResult(w->h, &w->rx[i].ov, &n, FALSE);
        else
            idle = 0;
    }
    if (w->started && !w->is_comm)
        op_logf("win: at most %u of the %u posted reads were complete when the "
                "reader came for the oldest", w->waiting_most, w->nreads);
    if (w->h != INVALID_HANDLE_VALUE) CloseHandle(w->h);
    if (!idle || w->wr_stuck) {
        op_logf("close: an I/O never returned; its buffers are left allocated");
        return;
    }
    for (i = 0; w->rx != NULL && i < w->nreads; i++)
        if (w->rx[i].ov.hEvent != NULL) CloseHandle(w->rx[i].ov.hEvent);
    if (w->wr.hEvent != NULL) CloseHandle(w->wr.hEvent);
    free(w->rx);
    free(w);
}

static void win_close(op_transport *t)
{
    if (t == NULL || t->impl == NULL) return;
    win_free((op_win *)t->impl);
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
     * usbser.sys honours it (Windows 10, 2026-11-11).
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

static int rx_alloc(op_win *w)
{
    unsigned i;

    w->wr.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    w->rx = (rx_slot *)calloc(w->nreads, sizeof *w->rx);
    if (w->wr.hEvent == NULL || w->rx == NULL) return 0;
    for (i = 0; i < w->nreads; i++) {
        w->rx[i].ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (w->rx[i].ov.hEvent == NULL) return 0;
    }
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
    if (w == NULL) {
        CloseHandle(h);
        set_detail("out of memory");
        return OP_ERR_IO;
    }
    w->h = h;
    w->is_comm = comm_setup(h);
    w->nreads = w->is_comm ? 1u : WIN_RX_READS;
    if (!rx_alloc(w)) {
        win_free(w);
        set_detail("out of memory");
        return OP_ERR_IO;
    }

    /* The system timer ticks every 15.6 ms unless asked otherwise, and every
     * timed wait in the library would be rounded up to it. */
    timeBeginPeriod(1);

    out->ops  = &g_win_ops;
    out->impl = w;
    op_logf("open: %s (%s, %u reads posted)", path,
            w->is_comm ? "serial port" : "device interface", w->nreads);
    return OP_OK;
}

op_status op_serial_open(op_transport *out, const char *path)
{
    char full[610];

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

static int hwid_has(HDEVINFO set, SP_DEVINFO_DATA *dev, const char *id)
{
    char ids[1024];
    DWORD type = 0, len = 0;
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_HARDWAREID, &type,
                                           (BYTE *)ids, sizeof ids, &len))
        return 0;
    return op_win_has_id(ids, len < sizeof ids ? len : sizeof ids, id);
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
        if (op_win_has_id(detail.d.DevicePath, strlen(detail.d.DevicePath), OP_HWID)
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
    char dev_buf[600];
    const char *dev = op_getenv("OPENPORT_DEVICE", dev_buf, sizeof dev_buf);
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
