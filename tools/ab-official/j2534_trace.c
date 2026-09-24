/*
 * j2534_trace — drive any J2534 library through named scenarios and record
 * every call as one JSON line: arguments, return code, outputs, elapsed time.
 *
 * The same source builds for Win32 (i686-w64-mingw32-gcc, __stdcall, loads a
 * .dll) and for macOS/Linux (cc, dlopen). Run it once against the vendor DLL
 * under Wine and once against libj2534.dylib, and diff the two traces with
 * ab_diff.py. Nothing here knows which library it is driving.
 *
 *   j2534_trace <library> <trace.jsonl> [scenario ...] [--repeat N] [--cycles N]
 *   j2534_trace <library> <trace.jsonl> --replay <record>
 *
 * --replay executes a call recording made by the driver (OPENPORT_RECORD=file,
 * src/op_j2534.c) against whichever library is given, mapping the device,
 * channel, filter and periodic ids the recording saw onto the ones this
 * library hands out. Two replays of one recording through two libraries are
 * the same application asking for the same things; the wire taps then show
 * whether the libraries did the same things.
 *
 * Scenarios: open iso15765 multiframe timeouts errors periodic raw_can kline bench
 * sweep_connect sweep_config sweep_filter sweep_tx ("sweep" runs all four) edge
 * (default: all but kline, bench, the sweeps and edge).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include "wineshim.h"
#  define J2534_CALL __stdcall
#else
#  define _DARWIN_C_SOURCE 1
#  include <dlfcn.h>
#  include <time.h>
#  include <unistd.h>
#  define J2534_CALL
#endif

#include "j2534/j2534.h"

typedef long (J2534_CALL *fn_open)(const void *, J_U32 *);
typedef long (J2534_CALL *fn_close)(J_U32);
typedef long (J2534_CALL *fn_connect)(J_U32, J_U32, J_U32, J_U32, J_U32 *);
typedef long (J2534_CALL *fn_disconnect)(J_U32);
typedef long (J2534_CALL *fn_read)(J_U32, PASSTHRU_MSG *, J_U32 *, J_U32);
typedef long (J2534_CALL *fn_write)(J_U32, const PASSTHRU_MSG *, J_U32 *, J_U32);
typedef long (J2534_CALL *fn_startp)(J_U32, const PASSTHRU_MSG *, J_U32 *, J_U32);
typedef long (J2534_CALL *fn_stopp)(J_U32, J_U32);
typedef long (J2534_CALL *fn_filter)(J_U32, J_U32, const PASSTHRU_MSG *, const PASSTHRU_MSG *,
                                     const PASSTHRU_MSG *, J_U32 *);
typedef long (J2534_CALL *fn_stopf)(J_U32, J_U32);
typedef long (J2534_CALL *fn_progv)(J_U32, J_U32, J_U32);
typedef long (J2534_CALL *fn_version)(J_U32, char *, char *, char *);
typedef long (J2534_CALL *fn_lasterr)(char *);
typedef long (J2534_CALL *fn_ioctl)(J_U32, J_U32, const void *, void *);

static struct {
    fn_open open; fn_close close; fn_connect connect; fn_disconnect disconnect;
    fn_read read; fn_write write; fn_startp startp; fn_stopp stopp;
    fn_filter filter; fn_stopf stopf; fn_progv progv; fn_version version;
    fn_lasterr lasterr; fn_ioctl ioctl;
} api;

static FILE *out;
static int   seq;
static int   mark_pending;     /* set by mark() (sweep section), consumed by record() */

/* ---- platform ---------------------------------------------------------- */
#ifdef _WIN32
static HMODULE lib;
static LARGE_INTEGER qpf;
static double now_us(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1e6 / (double)qpf.QuadPart; }
static void  sleep_ms(int ms) { Sleep((DWORD)ms); }
static void *sym(const char *n) { return (void *)GetProcAddress(lib, n); }
static int   load(const char *p) { QueryPerformanceFrequency(&qpf); lib = LoadLibraryA(p); return lib != NULL; }
#else
static void *lib;
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3; }
static void  sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
static void *sym(const char *n) { return dlsym(lib, n); }
static int   load(const char *p) { lib = dlopen(p, RTLD_NOW | RTLD_LOCAL); if (!lib) fprintf(stderr, "dlopen: %s\n", dlerror()); return lib != NULL; }
#endif

/* ---- JSON output ------------------------------------------------------- */
static void json_str(const char *s)
{
    fputc('"', out);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c < 0x20 || c >= 0x7f) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
}

static void json_msg(const PASSTHRU_MSG *m)
{
    J_U32 i, n = m->DataSize;
    if (n > J2534_MSG_DATA_MAX) n = J2534_MSG_DATA_MAX;
    fprintf(out, "{\"proto\":%lu,\"rx\":%lu,\"tx\":%lu,\"ts\":%lu,\"size\":%lu,\"extra\":%lu,\"data\":\"",
            (unsigned long)m->ProtocolID, (unsigned long)m->RxStatus, (unsigned long)m->TxFlags,
            (unsigned long)m->Timestamp, (unsigned long)m->DataSize, (unsigned long)m->ExtraDataIndex);
    for (i = 0; i < n; i++) fprintf(out, "%02x", m->Data[i]);
    fputs("\"}", out);
}

/* One call = one line. `detail` is a preformatted JSON fragment (may be NULL). */
static long record(const char *scenario, const char *step, long rc, double t0, const char *detail)
{
    double dt = now_us() - t0;
    char err[256];
    err[0] = 0;
    fprintf(out, "{\"i\":%d,\"scenario\":\"%s\",\"step\":\"%s\",\"rc\":%ld,\"us\":%.0f",
            ++seq, scenario, step, rc, dt);
    if (mark_pending) { fprintf(out, ",\"mark\":%d", mark_pending); mark_pending = 0; }
    if (rc != 0 && api.lasterr) {
        memset(err, 0, sizeof err);
        api.lasterr(err);
        err[sizeof err - 1] = 0;
        fputs(",\"err\":", out); json_str(err);
    }
    if (detail && *detail) fprintf(out, ",%s", detail);
    fputs("}\n", out);
    fflush(out);
    return rc;
}

#define STEP(sc, name, call, detail) do { double _t0 = now_us(); long _rc = (call); record(sc, name, _rc, _t0, detail); } while (0)

/* ---- helpers ----------------------------------------------------------- */
static void mk_msg(PASSTHRU_MSG *m, J_U32 proto, J_U32 txflags, const unsigned char *d, J_U32 n)
{
    memset(m, 0, sizeof *m);
    m->ProtocolID = proto; m->TxFlags = txflags; m->DataSize = n;
    memcpy(m->Data, d, n);
}

static char dbuf[64 * 1024];

static const char *msgs_detail(const char *key, const PASSTHRU_MSG *m, J_U32 n)
{
    /* Build a JSON fragment "key":[msg,...] into dbuf via a temporary stream
     * swap: simpler than a second serialiser. */
    FILE *save = out;
    J_U32 i;
#ifdef _WIN32
    out = tmpfile();
#else
    out = fmemopen(dbuf, sizeof dbuf, "w");
#endif
    if (!out) { out = save; return NULL; }
    fprintf(out, "\"%s\":[", key);
    for (i = 0; i < n; i++) { if (i) fputc(',', out); json_msg(&m[i]); }
    fputc(']', out);
#ifdef _WIN32
    {
        long len;
        fflush(out); len = ftell(out); rewind(out);
        if (len < 0 || (size_t)len >= sizeof dbuf) len = 0;
        len = (long)fread(dbuf, 1, (size_t)len, out); dbuf[len] = 0;
    }
#endif
    fclose(out); out = save;
    return dbuf;
}

static long do_open(const char *sc, J_U32 *dev)
{
    long rc; double t0 = now_us(); char d[64];
    rc = api.open(NULL, dev);
    snprintf(d, sizeof d, "\"dev\":%lu", (unsigned long)*dev);
    return record(sc, "PassThruOpen", rc, t0, d);
}

static long do_connect(const char *sc, J_U32 dev, J_U32 proto, J_U32 baud, J_U32 *ch)
{
    long rc; double t0 = now_us(); char d[96];
    rc = api.connect(dev, proto, 0, baud, ch);
    snprintf(d, sizeof d, "\"proto\":%lu,\"baud\":%lu,\"ch\":%lu", (unsigned long)proto, (unsigned long)baud, (unsigned long)*ch);
    return record(sc, "PassThruConnect", rc, t0, d);
}

static long iso_setup(const char *sc, J_U32 ch, J_U32 *fid)
{
    SCONFIG cfg[2]; SCONFIG_LIST list; PASSTHRU_MSG mask, pat, fc; long rc; double t0; char d[64];
    cfg[0].Parameter = ISO15765_BS; cfg[0].Value = 0;
    cfg[1].Parameter = ISO15765_STMIN; cfg[1].Value = 0;
    list.NumOfParams = 2; list.ConfigPtr = cfg;
    STEP(sc, "Ioctl SET_CONFIG BS=0 STMIN=0", api.ioctl(ch, SET_CONFIG, &list, NULL), NULL);
    memset(&mask, 0, sizeof mask); memset(&pat, 0, sizeof pat); memset(&fc, 0, sizeof fc);
    mask.ProtocolID = pat.ProtocolID = fc.ProtocolID = ISO15765;
    mask.TxFlags = pat.TxFlags = fc.TxFlags = ISO15765_FRAME_PAD;
    mask.DataSize = pat.DataSize = fc.DataSize = 4;
    memset(mask.Data, 0xFF, 4);
    pat.Data[2] = 0x07; pat.Data[3] = 0xE8;
    fc.Data[2]  = 0x07; fc.Data[3]  = 0xE0;
    t0 = now_us();
    rc = api.filter(ch, FLOW_CONTROL_FILTER, &mask, &pat, &fc, fid);
    snprintf(d, sizeof d, "\"filter\":%lu", (unsigned long)*fid);
    record(sc, "StartMsgFilter FLOW_CONTROL 7E8/7E0", rc, t0, d);
    STEP(sc, "Ioctl CLEAR_RX_BUFFER", api.ioctl(ch, CLEAR_RX_BUFFER, NULL, NULL), NULL);
    return rc;
}

static long write_one(const char *sc, const char *name, J_U32 ch, J_U32 proto, J_U32 flags,
                      const unsigned char *bytes, J_U32 n, J_U32 timeout)
{
    PASSTHRU_MSG m; J_U32 cnt = 1; long rc; double t0; char d[48];
    mk_msg(&m, proto, flags, bytes, n);
    t0 = now_us();
    rc = api.write(ch, &m, &cnt, timeout);
    snprintf(d, sizeof d, "\"sent\":%lu,\"timeout\":%lu", (unsigned long)cnt, (unsigned long)timeout);
    return record(sc, name, rc, t0, d);
}

static long read_some(const char *sc, const char *name, J_U32 ch, J_U32 want, J_U32 timeout)
{
    static PASSTHRU_MSG in[64];
    J_U32 cnt = want > 64 ? 64 : want; long rc; double t0; const char *d; char head[64];
    memset(in, 0, sizeof in);
    t0 = now_us();
    rc = api.read(ch, in, &cnt, timeout);
    if (cnt > 64) cnt = 0;
    d = msgs_detail("msgs", in, cnt);
    snprintf(head, sizeof head, "\"timeout\":%lu,\"received\":%lu,", (unsigned long)timeout, (unsigned long)cnt);
    if (d) {
        size_t hl = strlen(head), dl = strlen(d);
        if (hl + dl + 1 < sizeof dbuf) { memmove(dbuf + hl, dbuf, dl + 1); memcpy(dbuf, head, hl); }
        return record(sc, name, rc, t0, dbuf);
    }
    head[strlen(head) - 1] = 0;
    return record(sc, name, rc, t0, head);
}

/* ---- scenarios --------------------------------------------------------- */
static const unsigned char TESTER_PRESENT[] = { 0x00, 0x00, 0x07, 0xE0, 0x3E, 0x00 };
static const unsigned char READ_DID_0100[]  = { 0x00, 0x00, 0x07, 0xE0, 0x22, 0x01, 0x00 };
static const unsigned char RAW_FRAME[]      = { 0x00, 0x00, 0x07, 0xDF, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static void sc_open(void)
{
    const char *sc = "open";
    J_U32 dev = 0, v = 0; char fw[80], dl[80], ap[80], d[300]; long rc; double t0;
    if (do_open(sc, &dev) != 0) return;
    memset(fw, 0, sizeof fw); memset(dl, 0, sizeof dl); memset(ap, 0, sizeof ap);
    t0 = now_us(); rc = api.version(dev, fw, dl, ap);
    snprintf(d, sizeof d, "\"firmware\":\"%.79s\",\"dll\":\"%.79s\",\"api\":\"%.79s\"", fw, dl, ap);
    record(sc, "PassThruReadVersion", rc, t0, d);
    t0 = now_us(); rc = api.ioctl(dev, READ_VBATT, NULL, &v);
    snprintf(d, sizeof d, "\"vbatt\":%lu", (unsigned long)v);
    record(sc, "Ioctl READ_VBATT", rc, t0, d);
    STEP(sc, "PassThruClose", api.close(dev), NULL);
}

static void sc_iso15765(void)
{
    const char *sc = "iso15765";
    J_U32 dev = 0, ch = 0, fid = 0;
    if (do_open(sc, &dev) != 0) return;
    if (do_connect(sc, dev, ISO15765, 500000, &ch) == 0) {
        iso_setup(sc, ch, &fid);
        write_one(sc, "WriteMsgs $3E TesterPresent", ch, ISO15765, ISO15765_FRAME_PAD, TESTER_PRESENT, sizeof TESTER_PRESENT, 1000);
        read_some(sc, "ReadMsgs 500ms", ch, 4, 500);
        STEP(sc, "StopMsgFilter", api.stopf(ch, fid), NULL);
        STEP(sc, "PassThruDisconnect", api.disconnect(ch), NULL);
    }
    STEP(sc, "PassThruClose", api.close(dev), NULL);
}

static void sc_multiframe(void)
{
    const char *sc = "multiframe";
    J_U32 dev = 0, ch = 0, fid = 0;
    if (do_open(sc, &dev) != 0) return;
    if (do_connect(sc, dev, ISO15765, 500000, &ch) == 0) {
        iso_setup(sc, ch, &fid);
        write_one(sc, "WriteMsgs $22 0100", ch, ISO15765, ISO15765_FRAME_PAD, READ_DID_0100, sizeof READ_DID_0100, 1000);
        read_some(sc, "ReadMsgs 1500ms (600-byte reply)", ch, 4, 1500);
        STEP(sc, "PassThruDisconnect", api.disconnect(ch), NULL);
    }
    STEP(sc, "PassThruClose", api.close(dev), NULL);
}

static void sc_timeouts(void)
{
    const char *sc = "timeouts";
    J_U32 dev = 0, ch = 0, fid = 0;
    if (do_open(sc, &dev) != 0) return;
    if (do_connect(sc, dev, ISO15765, 500000, &ch) == 0) {
        iso_setup(sc, ch, &fid);
        read_some(sc, "ReadMsgs idle 0ms", ch, 1, 0);
        read_some(sc, "ReadMsgs idle 100ms", ch, 1, 100);
        read_some(sc, "ReadMsgs idle 500ms", ch, 1, 500);
        write_one(sc, "WriteMsgs $3E timeout 0", ch, ISO15765, ISO15765_FRAME_PAD, TESTER_PRESENT, sizeof TESTER_PRESENT, 0);
        read_some(sc, "ReadMsgs after write 300ms", ch, 4, 300);
        STEP(sc, "PassThruDisconnect", api.disconnect(ch), NULL);
    }
    STEP(sc, "PassThruClose", api.close(dev), NULL);
}

static void sc_errors(void)
{
    const char *sc = "errors";
    J_U32 dev = 0, ch = 0, v = 0, f2 = 0; PASSTHRU_MSG pat, fc;
    STEP(sc, "Disconnect before open (ch 9)", api.disconnect(9), NULL);
    if (do_open(sc, &dev) != 0) return;
    STEP(sc, "Ioctl unknown 0xDEAD on device", api.ioctl(dev, 0xDEAD, NULL, NULL), NULL);
    if (do_connect(sc, dev, ISO15765, 500000, &ch) == 0) {
        STEP(sc, "Connect ISO15765 twice", api.connect(dev, ISO15765, 0, 500000, &v), NULL);
        STEP(sc, "Ioctl unknown 0xDEAD on channel", api.ioctl(ch, 0xDEAD, NULL, NULL), NULL);
        memset(&pat, 0, sizeof pat); memset(&fc, 0, sizeof fc);
        pat.ProtocolID = fc.ProtocolID = ISO15765; pat.DataSize = fc.DataSize = 4;
        STEP(sc, "StartMsgFilter NULL mask", api.filter(ch, FLOW_CONTROL_FILTER, NULL, &pat, &fc, &f2), NULL);
        STEP(sc, "StopMsgFilter bogus 77", api.stopf(ch, 77), NULL);
        STEP(sc, "StopPeriodicMsg bogus 77", api.stopp(ch, 77), NULL);
        STEP(sc, "Ioctl GET_CONFIG unsupported param 0x7F", ({ SCONFIG c = { 0x7F, 0 }; SCONFIG_LIST l = { 1, &c }; api.ioctl(ch, GET_CONFIG, &l, NULL); }), NULL);
        STEP(sc, "SetProgrammingVoltage pin 12 off", api.progv(dev, 12, VOLTAGE_OFF), NULL);
        STEP(sc, "PassThruDisconnect", api.disconnect(ch), NULL);
        STEP(sc, "Disconnect twice", api.disconnect(ch), NULL);
    }
    STEP(sc, "PassThruClose", api.close(dev), NULL);
    STEP(sc, "Close twice", api.close(dev), NULL);
}

static void sc_periodic(void)
{
    const char *sc = "periodic";
    J_U32 dev = 0, ch = 0, fid = 0, id = 0; PASSTHRU_MSG m; long rc; double t0; char d[48];
    if (do_open(sc, &dev) != 0) return;
    if (do_connect(sc, dev, ISO15765, 500000, &ch) == 0) {
        iso_setup(sc, ch, &fid);
        mk_msg(&m, ISO15765, ISO15765_FRAME_PAD, TESTER_PRESENT, sizeof TESTER_PRESENT);
        t0 = now_us(); rc = api.startp(ch, &m, &id, 100);
        snprintf(d, sizeof d, "\"msgid\":%lu,\"interval\":100", (unsigned long)id);
        record(sc, "StartPeriodicMsg 100ms", rc, t0, d);
        sleep_ms(550);
        read_some(sc, "ReadMsgs after 550ms", ch, 8, 100);
        STEP(sc, "StopPeriodicMsg", api.stopp(ch, id), NULL);
        STEP(sc, "PassThruDisconnect", api.disconnect(ch), NULL);
    }
    STEP(sc, "PassThruClose", api.close(dev), NULL);
}

static void sc_raw_can(void)
{
    const char *sc = "raw_can";
    J_U32 dev = 0, ch = 0, fid = 0; PASSTHRU_MSG mask, pat; long rc; double t0; char d[48];
    if (do_open(sc, &dev) != 0) return;
    if (do_connect(sc, dev, CAN, 500000, &ch) == 0) {
        memset(&mask, 0, sizeof mask); memset(&pat, 0, sizeof pat);
        mask.ProtocolID = pat.ProtocolID = CAN; mask.DataSize = pat.DataSize = 4;
        t0 = now_us(); rc = api.filter(ch, PASS_FILTER, &mask, &pat, NULL, &fid);
        snprintf(d, sizeof d, "\"filter\":%lu", (unsigned long)fid);
        record(sc, "StartMsgFilter PASS all", rc, t0, d);
        write_one(sc, "WriteMsgs raw 7DF 02 01 00", ch, CAN, 0, RAW_FRAME, sizeof RAW_FRAME, 1000);
        read_some(sc, "ReadMsgs 500ms", ch, 4, 500);
        STEP(sc, "PassThruDisconnect", api.disconnect(ch), NULL);
    }
    STEP(sc, "PassThruClose", api.close(dev), NULL);
}


/* K-line: what each driver sends for the two J2534 inits, and what comes back.
 * Fast init to the functional address and to the VAG engine address on
 * ISO14230, then a five-baud sweep of the usual VAG module addresses on
 * ISO9141. On a bench nothing answers; on a car with a live K-line this is
 * the first evidence of which driver, if either, the ECU talks to. */
static const unsigned char SWEEP[] = { 0x33, 0x01, 0x10, 0x17, 0x19, 0x25, 0x08, 0x46 };
static void sc_kline(void)
{
    const char *sc = "kline";
    J_U32 dev = 0, ch = 0; unsigned i; char name[64], d[128];
    if (do_open(sc, &dev) != 0) return;
    if (do_connect(sc, dev, ISO14230, 10400, &ch) == 0) {
        static const unsigned char starts[2][4] = { { 0xC1, 0x33, 0xF1, 0x81 }, { 0x81, 0x01, 0xF1, 0x81 } };
        for (i = 0; i < 2; i++) {
            PASSTHRU_MSG req, rsp; long rc; double t0; const char *md;
            memset(&req, 0, sizeof req); memset(&rsp, 0, sizeof rsp);
            req.ProtocolID = ISO14230; req.DataSize = 4; memcpy(req.Data, starts[i], 4);
            snprintf(name, sizeof name, "Ioctl FAST_INIT StartCommunication to %s", i ? "0x01 physical" : "0x33 functional");
            t0 = now_us(); rc = api.ioctl(ch, FAST_INIT, &req, &rsp);
            md = msgs_detail("response", &rsp, rc == 0 ? 1 : 0);
            record(sc, name, rc, t0, md);
            read_some(sc, i ? "ReadMsgs after fast init 0x01 500ms" : "ReadMsgs after fast init 0x33 500ms", ch, 4, 500);
        }
        STEP(sc, "PassThruDisconnect ISO14230", api.disconnect(ch), NULL);
    }
    if (do_connect(sc, dev, ISO9141, 10400, &ch) == 0) {
        for (i = 0; i < sizeof SWEEP; i++) {
            unsigned char addr = SWEEP[i], keys[16]; SBYTE_ARRAY in, outb; long rc; double t0; unsigned k;
            in.NumOfBytes = 1; in.BytePtr = &addr; outb.NumOfBytes = sizeof keys; outb.BytePtr = keys;
            snprintf(name, sizeof name, "Ioctl FIVE_BAUD_INIT address 0x%02X", addr);
            t0 = now_us(); rc = api.ioctl(ch, FIVE_BAUD_INIT, &in, &outb);
            snprintf(d, sizeof d, "\"keybytes\":\"");
            for (k = 0; rc == 0 && k < outb.NumOfBytes && k < sizeof keys; k++)
                snprintf(d + strlen(d), sizeof d - strlen(d), "%02x", keys[k]);
            strncat(d, "\"", sizeof d - strlen(d) - 1);
            record(sc, name, rc, t0, d);
        }
        STEP(sc, "PassThruDisconnect ISO9141", api.disconnect(ch), NULL);
    }
    STEP(sc, "PassThruClose", api.close(dev), NULL);
}

static int bench_cycles = 20;
static void sc_bench(void)
{
    const char *sc = "bench";
    J_U32 dev = 0, ch = 0, fid = 0; int i; char name[48];
    if (do_open(sc, &dev) != 0) return;
    if (do_connect(sc, dev, ISO15765, 500000, &ch) == 0) {
        iso_setup(sc, ch, &fid);
        for (i = 0; i < bench_cycles; i++) {
            snprintf(name, sizeof name, "bench write %d", i);
            write_one(sc, name, ch, ISO15765, ISO15765_FRAME_PAD, TESTER_PRESENT, sizeof TESTER_PRESENT, 1000);
            snprintf(name, sizeof name, "bench read %d", i);
            read_some(sc, name, ch, 4, 500);
        }
        STEP(sc, "PassThruDisconnect", api.disconnect(ch), NULL);
    }
    STEP(sc, "PassThruClose", api.close(dev), NULL);
}


/* ---- replay ------------------------------------------------------------ */
#define MAP_MAX 64
typedef struct { J_U32 rec, live; } idmap;
static struct { idmap dev[4], ch[16], filt[MAP_MAX], per[16]; int ndev, nch, nfilt, nper; } maps;
static J_U32 map_get(const idmap *m, int n, J_U32 rec, int *found)
{
    int i; for (i = 0; i < n; i++) if (m[i].rec == rec) { *found = 1; return m[i].live; }
    *found = 0; return rec;
}
static void map_put(idmap *m, int *n, int cap, J_U32 rec, J_U32 live)
{
    int i; for (i = 0; i < *n; i++) if (m[i].rec == rec) { m[i].live = live; return; }
    if (*n < cap) { m[*n].rec = rec; m[*n].live = live; (*n)++; }
}
static J_U32 map_ch(J_U32 rec)
{
    int f; J_U32 v = map_get(maps.ch, maps.nch, rec, &f);
    if (!f) fprintf(stderr, "replay: channel %lu was never mapped (no '= id' line after connect?); using it as is\n", (unsigned long)rec);
    return v;
}
static J_U32 map_dev(J_U32 rec)
{
    int f; J_U32 v = map_get(maps.dev, maps.ndev, rec, &f);
    if (!f) fprintf(stderr, "replay: device %lu was never mapped; using it as is\n", (unsigned long)rec);
    return v;
}
static J_U32 map_any(J_U32 rec) { int f; J_U32 v = map_get(maps.ch, maps.nch, rec, &f); if (f) return v; return map_get(maps.dev, maps.ndev, rec, &f); }

static int hex_to_bytes(const char *h, unsigned char *out, size_t cap)
{
    size_t n = 0;
    if (!strcmp(h, "-")) return 0;
    while (h[0] && h[1] && n < cap) {
        unsigned v; if (sscanf(h, "%2x", &v) != 1) break;
        out[n++] = (unsigned char)v; h += 2;
    }
    return (int)n;
}

/* Parse " <proto> <txflags> <hex>" triples from p into m[]; returns count. */
static int parse_msgs(char *p, PASSTHRU_MSG *m, int cap)
{
    int n = 0;
    char *tok;
    while (n < cap) {
        char *proto = strtok_r(p, " \t\r\n", &tok); p = NULL;
        char *flags = proto ? strtok_r(NULL, " \t\r\n", &tok) : NULL;
        char *hex   = flags ? strtok_r(NULL, " \t\r\n", &tok) : NULL;
        if (!hex) break;
        if (!strcmp(proto, "-")) { n++; continue; }              /* a NULL message */
        memset(&m[n], 0, sizeof m[n]);
        m[n].ProtocolID = strtoul(proto, NULL, 10);
        m[n].TxFlags = strtoul(flags, NULL, 10);
        m[n].DataSize = (J_U32)hex_to_bytes(hex, m[n].Data, sizeof m[n].Data);
        n++;
        p = tok;
        if (!p) break;
    }
    return n;
}

static void replay(const char *path)
{
    static char line[70000];
    static PASSTHRU_MSG msgs[64];
    FILE *f = fopen(path, "r");
    int lineno = 0;
    enum { NONE, OPEN, CONNECT, FILTER, PERIODIC } last = NONE;
    J_U32 last_live = 0;
    if (!f) { perror(path); return; }
    while (fgets(line, sizeof line, f)) {
        char sc[24], step[48], detail[128];
        char *save = NULL, *verb;
        unsigned long a = 0, b = 0, c = 0, d = 0;
        double t0;
        long rc;
        lineno++;
        snprintf(sc, sizeof sc, "replay");
        verb = strtok_r(line, " \t\r\n", &save);
        if (!verb) continue;
        if (!strcmp(verb, "=")) {           /* recorded result id -> map to the live one */
            char *v = strtok_r(NULL, " \t\r\n", &save);
            J_U32 rec = v ? strtoul(v, NULL, 10) : 0;
            if (last == OPEN) map_put(maps.dev, &maps.ndev, 4, rec, last_live);
            else if (last == CONNECT) map_put(maps.ch, &maps.nch, 16, rec, last_live);
            else if (last == FILTER) map_put(maps.filt, &maps.nfilt, MAP_MAX, rec, last_live);
            else if (last == PERIODIC) map_put(maps.per, &maps.nper, 16, rec, last_live);
            last = NONE;
            continue;
        }
        snprintf(step, sizeof step, "L%d %s", lineno, verb);
        last = NONE;
        if (!strcmp(verb, "open")) {
            J_U32 dev = 0; t0 = now_us(); rc = api.open(NULL, &dev);
            snprintf(detail, sizeof detail, "\"dev\":%lu", (unsigned long)dev);
            record(sc, step, rc, t0, detail); last = OPEN; last_live = dev;
        } else if (!strcmp(verb, "sleep")) {       /* hand-written probes: let traffic queue */
            sscanf(save, "%lu", &a); sleep_ms((int)a);
        } else if (!strcmp(verb, "close")) {
            sscanf(save, "%lu", &a); t0 = now_us(); record(sc, step, api.close(map_dev(a)), t0, NULL);
        } else if (!strcmp(verb, "version")) {
            char fw[80] = "", dl[80] = "", ap[80] = "";
            sscanf(save, "%lu", &a); t0 = now_us(); rc = api.version(map_dev(a), fw, dl, ap);
            snprintf(detail, sizeof detail, "\"firmware\":\"%.40s\"", fw); record(sc, step, rc, t0, detail);
        } else if (!strcmp(verb, "lasterr")) {
            char err[256] = ""; t0 = now_us(); record(sc, step, api.lasterr(err), t0, NULL);
        } else if (!strcmp(verb, "connect")) {
            J_U32 ch = 0; sscanf(save, "%lu %lu %lu %lu", &a, &b, &c, &d);
            t0 = now_us(); rc = api.connect(map_dev(a), b, c, d, &ch);
            snprintf(detail, sizeof detail, "\"proto\":%lu,\"ch\":%lu", b, (unsigned long)ch);
            record(sc, step, rc, t0, detail); last = CONNECT; last_live = ch;
        } else if (!strcmp(verb, "disconnect")) {
            sscanf(save, "%lu", &a); t0 = now_us(); record(sc, step, api.disconnect(map_ch(a)), t0, NULL);
        } else if (!strcmp(verb, "read")) {
            sscanf(save, "%lu %lu %lu", &a, &b, &c);
            read_some(sc, step, map_ch(a), (J_U32)b, (J_U32)c);
        } else if (!strcmp(verb, "write")) {
            char *rest; int n; J_U32 cnt;
            a = strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            b = strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            c = strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            rest = save;
            n = parse_msgs(rest, msgs, 64);
            cnt = (J_U32)(b < (unsigned long)n ? b : (unsigned long)n);
            t0 = now_us(); rc = api.write(map_ch(a), msgs, &cnt, (J_U32)c);
            snprintf(detail, sizeof detail, "\"sent\":%lu,\"timeout\":%lu,\"bytes\":%lu", (unsigned long)cnt, c, n ? (unsigned long)msgs[0].DataSize : 0UL);
            record(sc, step, rc, t0, detail);
        } else if (!strcmp(verb, "startp")) {
            J_U32 id = 0;
            a = strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            b = strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            parse_msgs(save, msgs, 1);
            t0 = now_us(); rc = api.startp(map_ch(a), &msgs[0], &id, (J_U32)b);
            snprintf(detail, sizeof detail, "\"msgid\":%lu", (unsigned long)id);
            record(sc, step, rc, t0, detail); last = PERIODIC; last_live = id;
        } else if (!strcmp(verb, "stopp")) {
            int fnd; sscanf(save, "%lu %lu", &a, &b);
            t0 = now_us(); record(sc, step, api.stopp(map_ch(a), map_get(maps.per, maps.nper, b, &fnd)), t0, NULL);
        } else if (!strcmp(verb, "filter")) {
            J_U32 fid = 0; int n;
            a = strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            b = strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            n = parse_msgs(save, msgs, 3);
            t0 = now_us();
            rc = api.filter(map_ch(a), b, n > 0 && msgs[0].ProtocolID ? &msgs[0] : NULL,
                            n > 1 && msgs[1].ProtocolID ? &msgs[1] : NULL,
                            n > 2 && msgs[2].ProtocolID ? &msgs[2] : NULL, &fid);
            snprintf(detail, sizeof detail, "\"filter\":%lu", (unsigned long)fid);
            record(sc, step, rc, t0, detail); last = FILTER; last_live = fid;
        } else if (!strcmp(verb, "stopf")) {
            int fnd; sscanf(save, "%lu %lu", &a, &b);
            t0 = now_us(); record(sc, step, api.stopf(map_ch(a), map_get(maps.filt, maps.nfilt, b, &fnd)), t0, NULL);
        } else if (!strcmp(verb, "progv")) {
            sscanf(save, "%lu %lu %lu", &a, &b, &c); t0 = now_us(); record(sc, step, api.progv(map_dev(a), b, c), t0, NULL);
        } else if (!strcmp(verb, "ioctl")) {
            SCONFIG cfg[32]; SCONFIG_LIST list; J_U32 v = 0, pin = 0; void *in = NULL, *out = NULL; int np = 0;
            char *tok;
            a = strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            b = strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            while ((tok = strtok_r(NULL, " \t\r\n", &save)) && np < 32) {
                unsigned long pp = 0, vv = 0; sscanf(tok, "%lu=%lu", &pp, &vv);
                cfg[np].Parameter = pp; cfg[np].Value = vv; np++;
            }
            if (b == SET_CONFIG || b == GET_CONFIG) { list.NumOfParams = (J_U32)np; list.ConfigPtr = cfg; in = &list; }
            if (b == READ_VBATT || b == READ_PROG_VOLTAGE) out = &v;
            if (b == READ_PROG_VOLTAGE && np > 0) { pin = (J_U32)cfg[0].Parameter; in = &pin; }
            t0 = now_us(); rc = api.ioctl(map_any(a), b, in, out);
            snprintf(detail, sizeof detail, "\"ioctl\":%lu,\"vbatt\":%lu", b, (unsigned long)v);
            record(sc, step, rc, t0, detail);
        }
    }
    fclose(f);
}

/* ---- sweep --------------------------------------------------------------
 * The parameters the scenarios above never pass, each once: connect flags and
 * rates, every configuration parameter, filter shapes and flags, transmit
 * flags and sizes, periodic and K-line init variants. The vendor DLL's A/B
 * finds so far were all one parameter translated differently, so this is
 * where the next ones are. Every step is preceded by a marker call that both
 * libraries put on the wire identically, `atr 17` (READ_PROG_VOLTAGE on
 * PIN_VADJ with the pin in pInput, PROTOCOL.md section 8.1); ab_diff.py cuts
 * the wire at the markers and compares each step's commands. Transmits get a
 * 100 ms budget so a bench cable, where nothing acknowledges, fails them fast,
 * and carry only read-only requests (0x22, 0x3E, OBD mode 1), so a run with the
 * cable on a car changes nothing in it. */
static J_U32 mark_dev;
static int   marks;

static void mark(void)
{
    J_U32 pin = PIN_VADJ, v = 0;
    api.ioctl(mark_dev, READ_PROG_VOLTAGE, &pin, &v);
    mark_pending = ++marks;
}

static PASSTHRU_MSG *hexmsg(PASSTHRU_MSG *m, J_U32 proto, J_U32 flags, const char *hex)
{
    if (hex == NULL) return NULL;
    memset(m, 0, sizeof *m);
    m->ProtocolID = proto; m->TxFlags = flags;
    m->DataSize = (J_U32)hex_to_bytes(hex, m->Data, sizeof m->Data);
    return m;
}

static long sw_connect(const char *sc, const char *name, J_U32 proto, J_U32 flags, J_U32 baud, J_U32 *ch)
{
    long rc; double t0; char d[128];
    *ch = 0; mark(); t0 = now_us();
    rc = api.connect(mark_dev, proto, flags, baud, ch);
    snprintf(d, sizeof d, "\"proto\":%lu,\"flags\":%lu,\"baud\":%lu,\"ch\":%lu",
             (unsigned long)proto, (unsigned long)flags, (unsigned long)baud, (unsigned long)*ch);
    return record(sc, name, rc, t0, d);
}

static void sw_disconnect(const char *sc, const char *what, J_U32 ch)
{
    char name[96];
    snprintf(name, sizeof name, "%s Disconnect", what);
    mark(); STEP(sc, name, api.disconnect(ch), NULL);
}

#define UNTOUCHED 0xDEADBEEFUL   /* GET_CONFIG input value; still there means never written */

static long sw_config(const char *sc, const char *name, J_U32 ch, J_U32 ioctl_id, const SCONFIG *in, J_U32 n)
{
    SCONFIG cfg[8]; SCONFIG_LIST list; long rc; double t0; char d[256]; J_U32 i;
    if (n > 8) n = 8;
    memcpy(cfg, in, n * sizeof *cfg);
    list.NumOfParams = n; list.ConfigPtr = cfg;
    mark(); t0 = now_us();
    rc = api.ioctl(ch, ioctl_id, &list, NULL);
    snprintf(d, sizeof d, "\"values\":\"");
    for (i = 0; i < n; i++)
        snprintf(d + strlen(d), sizeof d - strlen(d), "%s%lu=%lu", i ? "," : "",
                 (unsigned long)cfg[i].Parameter, (unsigned long)cfg[i].Value);
    strncat(d, "\"", sizeof d - strlen(d) - 1);
    return record(sc, name, rc, t0, d);
}

static long sw_ioctl(const char *sc, const char *name, J_U32 ch, J_U32 id, const void *in, void *outp)
{
    long rc; double t0;
    mark(); t0 = now_us();
    rc = api.ioctl(ch, id, in, outp);
    return record(sc, name, rc, t0, NULL);
}

/* Starts a filter; unless keep, stops it again at once so every case meets an
 * empty channel. */
static long sw_filter(const char *sc, const char *name, J_U32 ch, J_U32 type, const PASSTHRU_MSG *mask,
                      const PASSTHRU_MSG *pat, const PASSTHRU_MSG *fc, int keep)
{
    long rc; double t0; char d[48], stop[128]; J_U32 fid = 0;
    mark(); t0 = now_us();
    rc = api.filter(ch, type, mask, pat, fc, &fid);
    snprintf(d, sizeof d, "\"filter\":%lu", (unsigned long)fid);
    record(sc, name, rc, t0, d);
    if (rc == 0 && !keep) {
        snprintf(stop, sizeof stop, "%s StopMsgFilter", name);
        mark(); STEP(sc, stop, api.stopf(ch, fid), NULL);
    }
    return rc;
}

static long sw_write(const char *sc, const char *name, J_U32 ch, const PASSTHRU_MSG *m, J_U32 n)
{
    long rc; double t0; char d[64]; J_U32 cnt = n;
    mark(); t0 = now_us();
    rc = api.write(ch, m, &cnt, 100);
    snprintf(d, sizeof d, "\"sent\":%lu,\"timeout\":100", (unsigned long)cnt);
    return record(sc, name, rc, t0, d);
}

/* Starts a periodic message; unless keep, stops it again at once. */
static long sw_periodic(const char *sc, const char *name, J_U32 ch, const PASSTHRU_MSG *m, J_U32 interval, int keep)
{
    long rc; double t0; char d[64], stop[128]; J_U32 id = 0;
    mark(); t0 = now_us();
    rc = api.startp(ch, m, &id, interval);
    snprintf(d, sizeof d, "\"msgid\":%lu,\"interval\":%lu", (unsigned long)id, (unsigned long)interval);
    record(sc, name, rc, t0, d);
    if (rc == 0 && !keep) {
        snprintf(stop, sizeof stop, "%s StopPeriodicMsg", name);
        mark(); STEP(sc, stop, api.stopp(ch, id), NULL);
    }
    return rc;
}

static int sw_open(const char *sc)
{
    if (do_open(sc, &mark_dev) != 0) return -1;
    return 0;
}

static void sw_close(const char *sc)
{
    mark(); STEP(sc, "PassThruClose", api.close(mark_dev), NULL);
}

static const struct { J_U32 proto, flags, baud; const char *name; } SW_CONNECTS[] = {
    { CAN, 0, 125000, "CAN 125000" }, { CAN, 0, 250000, "CAN 250000" }, { CAN, 0, 1000000, "CAN 1000000" },
    { CAN, 0, 33333, "CAN 33333" }, { CAN, 0, 12345, "CAN 12345" },
    { CAN, CAN_29BIT_ID, 500000, "CAN CAN_29BIT_ID" }, { CAN, CAN_ID_BOTH, 500000, "CAN CAN_ID_BOTH" },
    { CAN, CAN_29BIT_ID | CAN_ID_BOTH, 500000, "CAN CAN_29BIT_ID+CAN_ID_BOTH" },
    { CAN, 0x20000000, 500000, "CAN undefined flag 0x20000000" },
    { ISO15765, 0, 250000, "ISO15765 250000" }, { ISO15765, CAN_29BIT_ID, 500000, "ISO15765 CAN_29BIT_ID" },
    { ISO15765, CAN_ID_BOTH, 500000, "ISO15765 CAN_ID_BOTH" },
    { ISO14230, 0, 10400, "ISO14230 10400" }, { ISO14230, 0, 9600, "ISO14230 9600" },
    { ISO14230, 0, 4800, "ISO14230 4800" }, { ISO14230, ISO9141_NO_CHECKSUM, 10400, "ISO14230 ISO9141_NO_CHECKSUM" },
    { ISO14230, ISO9141_K_LINE_ONLY, 10400, "ISO14230 ISO9141_K_LINE_ONLY" },
    { ISO9141, 0, 10400, "ISO9141 10400" }, { ISO9141, 0, 4800, "ISO9141 4800" },
    { ISO9141, ISO9141_NO_CHECKSUM, 10400, "ISO9141 ISO9141_NO_CHECKSUM" },
    { ISO9141, ISO9141_K_LINE_ONLY, 10400, "ISO9141 ISO9141_K_LINE_ONLY" },
    { J1850VPW, 0, 10400, "J1850VPW 10400" }, { J1850PWM, 0, 41600, "J1850PWM 41600" },
    { CAN_CH1, 0, 500000, "CAN_CH1" }, { ISO15765_CH1, 0, 500000, "ISO15765_CH1" },
    { ISO9141_CH1, 0, 10400, "ISO9141_CH1" }, { ISO9141_CH2, 0, 10400, "ISO9141_CH2" },
    { ISO9141_CH3, 0, 19200, "ISO9141_CH3 19200" }, { ISO14230_CH1, 0, 10400, "ISO14230_CH1" },
    { ISO14230_CH2, 0, 10400, "ISO14230_CH2" },
};

static void sc_sweep_connect(void)
{
    const char *sc = "sweep_connect";
    unsigned i; J_U32 ch;
    if (sw_open(sc) != 0) return;
    for (i = 0; i < sizeof SW_CONNECTS / sizeof SW_CONNECTS[0]; i++)
        if (sw_connect(sc, SW_CONNECTS[i].name, SW_CONNECTS[i].proto, SW_CONNECTS[i].flags, SW_CONNECTS[i].baud, &ch) == 0)
            sw_disconnect(sc, SW_CONNECTS[i].name, ch);
    sw_close(sc);
}

/* Values are J2534-1 defaults, in J2534 units (P1_MAX, P3_MIN and P4_MIN in
 * 0.5 ms), so a driver that converts units and one that does not put
 * different numbers on the wire. DATA_RATE is set to the channel's own rate. */
static const struct { J_U32 id, value; const char *name; } SW_PARAMS[] = {
    { DATA_RATE, 0, "DATA_RATE" }, { LOOPBACK, 0, "LOOPBACK" }, { NODE_ADDRESS, 0, "NODE_ADDRESS" },
    { NETWORK_LINE, 0, "NETWORK_LINE" }, { P1_MIN, 0, "P1_MIN" }, { P1_MAX, 40, "P1_MAX" },
    { P2_MIN, 0, "P2_MIN" }, { P2_MAX, 0, "P2_MAX" }, { P3_MIN, 110, "P3_MIN" }, { P3_MAX, 0, "P3_MAX" },
    { P4_MIN, 10, "P4_MIN" }, { P4_MAX, 0, "P4_MAX" }, { W1, 300, "W1" }, { W2, 20, "W2" }, { W3, 20, "W3" },
    { W4, 50, "W4" }, { W5, 300, "W5" }, { TIDLE, 300, "TIDLE" }, { TINIL, 25, "TINIL" }, { TWUP, 50, "TWUP" },
    { PARITY, 0, "PARITY" }, { BIT_SAMPLE_POINT, 80, "BIT_SAMPLE_POINT" }, { SYNC_JUMP_WIDTH, 15, "SYNC_JUMP_WIDTH" },
    { W0, 300, "W0" }, { T1_MAX, 20, "T1_MAX" }, { T2_MAX, 100, "T2_MAX" }, { T4_MAX, 20, "T4_MAX" },
    { T5_MAX, 100, "T5_MAX" }, { ISO15765_BS, 0, "ISO15765_BS" }, { ISO15765_STMIN, 0, "ISO15765_STMIN" },
    { DATA_BITS, 0, "DATA_BITS" }, { FIVE_BAUD_MOD, 0, "FIVE_BAUD_MOD" }, { BS_TX, 0xFFFF, "BS_TX" },
    { STMIN_TX, 0xFFFF, "STMIN_TX" }, { T3_MAX, 50, "T3_MAX" }, { ISO15765_WFT_MAX, 0, "ISO15765_WFT_MAX" },
    { ISO15765_PAD_VALUE, 0, "ISO15765_PAD_VALUE" }, { CAN_MIXED_FORMAT, 0, "CAN_MIXED_FORMAT" },
    { TX_PARAM_STOP_BITS, 1, "TX_PARAM_STOP_BITS" },
};

static const struct { J_U32 proto, baud; const char *name; } SW_CHANNELS[] = {
    { ISO15765, 500000, "ISO15765" }, { CAN, 500000, "CAN" }, { ISO14230, 10400, "ISO14230" }, { ISO9141, 10400, "ISO9141" },
};

static void sc_sweep_config(void)
{
    const char *sc = "sweep_config";
    unsigned c, i; J_U32 ch; char name[96];
    if (sw_open(sc) != 0) return;
    for (c = 0; c < sizeof SW_CHANNELS / sizeof SW_CHANNELS[0]; c++) {
        const char *pn = SW_CHANNELS[c].name;
        snprintf(name, sizeof name, "%s Connect", pn);
        if (sw_connect(sc, name, SW_CHANNELS[c].proto, 0, SW_CHANNELS[c].baud, &ch) != 0) continue;
        for (i = 0; i < sizeof SW_PARAMS / sizeof SW_PARAMS[0]; i++) {
            SCONFIG get = { SW_PARAMS[i].id, UNTOUCHED }, set = { SW_PARAMS[i].id, SW_PARAMS[i].value };
            if (set.Parameter == DATA_RATE) set.Value = SW_CHANNELS[c].baud;
            snprintf(name, sizeof name, "%s %s get", pn, SW_PARAMS[i].name);
            sw_config(sc, name, ch, GET_CONFIG, &get, 1);
            snprintf(name, sizeof name, "%s %s set %lu", pn, SW_PARAMS[i].name, (unsigned long)set.Value);
            sw_config(sc, name, ch, SET_CONFIG, &set, 1);
            snprintf(name, sizeof name, "%s %s get after set", pn, SW_PARAMS[i].name);
            sw_config(sc, name, ch, GET_CONFIG, &get, 1);
        }
        if (SW_CHANNELS[c].proto == ISO15765) {
            SCONFIG r250 = { DATA_RATE, 250000 }, r500 = { DATA_RATE, 500000 }, g = { DATA_RATE, UNTOUCHED };
            SCONFIG gl[3] = { { DATA_RATE, UNTOUCHED }, { 0x7F, UNTOUCHED }, { LOOPBACK, UNTOUCHED } };
            SCONFIG sl[3] = { { LOOPBACK, 0 }, { 0x7F, 0 }, { ISO15765_BS, 0 } };
            SCONFIG lb1 = { LOOPBACK, 1 }, lb0 = { LOOPBACK, 0 }, glb = { LOOPBACK, UNTOUCHED };
            sw_config(sc, "ISO15765 DATA_RATE set 250000", ch, SET_CONFIG, &r250, 1);
            sw_config(sc, "ISO15765 DATA_RATE get after 250000", ch, GET_CONFIG, &g, 1);
            sw_config(sc, "ISO15765 DATA_RATE set 500000 again", ch, SET_CONFIG, &r500, 1);
            sw_config(sc, "ISO15765 LOOPBACK set 1", ch, SET_CONFIG, &lb1, 1);
            sw_config(sc, "ISO15765 LOOPBACK get after 1", ch, GET_CONFIG, &glb, 1);
            sw_config(sc, "ISO15765 LOOPBACK set 0", ch, SET_CONFIG, &lb0, 1);
            sw_config(sc, "ISO15765 GET list DATA_RATE,0x7F,LOOPBACK", ch, GET_CONFIG, gl, 3);
            sw_config(sc, "ISO15765 SET list LOOPBACK,0x7F,ISO15765_BS", ch, SET_CONFIG, sl, 3);
            sw_config(sc, "ISO15765 GET empty list", ch, GET_CONFIG, gl, 0);
            /* GET_CONFIG or SET_CONFIG with a NULL pInput is not swept: the
             * vendor DLL dereferences it and the process dies (page fault at
             * address 0, 1.02.0.4868, 2026-09-24). This driver answers
             * ERR_NULL_PARAMETER. */
            sw_config(sc, "GET_CONFIG DATA_RATE on the device id", mark_dev, GET_CONFIG, &g, 1);
        }
        snprintf(name, sizeof name, "%s", pn);
        sw_disconnect(sc, name, ch);
    }
    sw_close(sc);
}

/* One filter case: the message strings are hex, NULL for a NULL pointer. mproto
 * 0 means the channel's own protocol. */
typedef struct { const char *name; J_U32 type, mproto, mflags, fflags; const char *mask, *pat, *fc; } sw_fcase;

static const sw_fcase SW_CAN_FILTERS[] = {
    { "PASS 11-bit 7E8", PASS_FILTER, 0, 0, 0, "00000fff", "000007e8", NULL },
    { "BLOCK 11-bit 7E8", BLOCK_FILTER, 0, 0, 0, "00000fff", "000007e8", NULL },
    { "PASS 29-bit with CAN_29BIT_ID", PASS_FILTER, 0, CAN_29BIT_ID, 0, "1fffffff", "18daf110", NULL },
    { "PASS 29-bit without CAN_29BIT_ID", PASS_FILTER, 0, 0, 0, "1fffffff", "18daf110", NULL },
    { "PASS 1 byte", PASS_FILTER, 0, 0, 0, "00", "00", NULL },
    { "PASS 12 bytes", PASS_FILTER, 0, 0, 0, "00000fff0000000000000000", "000007e80000000000000000", NULL },
    { "PASS 13 bytes", PASS_FILTER, 0, 0, 0, "00000fff000000000000000000", "000007e8000000000000000000", NULL },
    { "PASS with a flow-control message", PASS_FILTER, 0, 0, 0, "00000fff", "000007e8", "000007e0" },
    { "FLOW_CONTROL on a CAN channel", FLOW_CONTROL_FILTER, 0, 0, 0, "ffffffff", "000007e8", "000007e0" },
    { "mask 4 bytes, pattern 5", PASS_FILTER, 0, 0, 0, "00000fff", "000007e800", NULL },
    { "NULL pattern", PASS_FILTER, 0, 0, 0, "00000fff", NULL, NULL },
    { "mask ProtocolID ISO15765", PASS_FILTER, ISO15765, 0, 0, "00000fff", "000007e8", NULL },
};

static const sw_fcase SW_ISO_FILTERS[] = {
    { "FC 11-bit, FRAME_PAD on all three", FLOW_CONTROL_FILTER, 0, ISO15765_FRAME_PAD, ISO15765_FRAME_PAD, "ffffffff", "000007e8", "000007e0" },
    { "FC 11-bit, FRAME_PAD on the FC message only", FLOW_CONTROL_FILTER, 0, 0, ISO15765_FRAME_PAD, "ffffffff", "000007e8", "000007e0" },
    { "FC 11-bit, FRAME_PAD on mask and pattern only", FLOW_CONTROL_FILTER, 0, ISO15765_FRAME_PAD, 0, "ffffffff", "000007e8", "000007e0" },
    { "FC 29-bit", FLOW_CONTROL_FILTER, 0, CAN_29BIT_ID | ISO15765_FRAME_PAD, CAN_29BIT_ID | ISO15765_FRAME_PAD, "1fffffff", "18daf110", "18da10f1" },
    { "FC extended addressing", FLOW_CONTROL_FILTER, 0, ISO15765_ADDR_TYPE | ISO15765_FRAME_PAD, ISO15765_ADDR_TYPE | ISO15765_FRAME_PAD, "ffffffffff", "00000612f1", "000006f112" },
    { "FC ISO15765_ADDR_TYPE with 4-byte messages", FLOW_CONTROL_FILTER, 0, ISO15765_ADDR_TYPE | ISO15765_FRAME_PAD, ISO15765_ADDR_TYPE | ISO15765_FRAME_PAD, "ffffffff", "000007e8", "000007e0" },
    { "FC with a NULL flow-control message", FLOW_CONTROL_FILTER, 0, ISO15765_FRAME_PAD, 0, "ffffffff", "000007e8", NULL },
    { "PASS on ISO15765", PASS_FILTER, 0, 0, 0, "ffffffff", "000007e8", NULL },
    { "BLOCK on ISO15765", BLOCK_FILTER, 0, 0, 0, "ffffffff", "000007e8", NULL },
    { "FC mask ProtocolID CAN", FLOW_CONTROL_FILTER, CAN, ISO15765_FRAME_PAD, ISO15765_FRAME_PAD, "ffffffff", "000007e8", "000007e0" },
};

static const sw_fcase SW_KLINE_FILTERS[] = {
    { "PASS 1 byte", PASS_FILTER, 0, 0, 0, "c0", "80", NULL },
    { "PASS 3 bytes", PASS_FILTER, 0, 0, 0, "ffffff", "80f110", NULL },
    { "BLOCK 1 byte", BLOCK_FILTER, 0, 0, 0, "c0", "80", NULL },
    { "FLOW_CONTROL on ISO14230", FLOW_CONTROL_FILTER, 0, 0, 0, "ffffff", "80f110", "8010f1" },
};

static void sw_filter_cases(const char *sc, const char *pn, J_U32 ch, J_U32 proto, const sw_fcase *cs, unsigned n)
{
    static PASSTHRU_MSG m, p, f;
    unsigned i; char name[128];
    for (i = 0; i < n; i++) {
        J_U32 mp = cs[i].mproto ? cs[i].mproto : proto;
        snprintf(name, sizeof name, "%s %s", pn, cs[i].name);
        sw_filter(sc, name, ch, cs[i].type, hexmsg(&m, mp, cs[i].mflags, cs[i].mask),
                  hexmsg(&p, mp, cs[i].mflags, cs[i].pat), hexmsg(&f, proto, cs[i].fflags, cs[i].fc), 0);
    }
}

static void sc_sweep_filter(void)
{
    const char *sc = "sweep_filter";
    static PASSTHRU_MSG m, p, f;
    J_U32 ch; unsigned i; char name[96], pat[16];
    if (sw_open(sc) != 0) return;
    if (sw_connect(sc, "CAN Connect", CAN, 0, 500000, &ch) == 0) {
        sw_filter_cases(sc, "CAN", ch, CAN, SW_CAN_FILTERS, sizeof SW_CAN_FILTERS / sizeof SW_CAN_FILTERS[0]);
        for (i = 0; i < 2; i++) {
            snprintf(name, sizeof name, "CAN PASS filter %u of 2", i + 1);
            snprintf(pat, sizeof pat, "000007%02x", 0xE0 + i);
            sw_filter(sc, name, ch, PASS_FILTER, hexmsg(&m, CAN, 0, "00000fff"), hexmsg(&p, CAN, 0, pat), NULL, 1);
        }
        sw_ioctl(sc, "CAN CLEAR_MSG_FILTERS with 2 set", ch, CLEAR_MSG_FILTERS, NULL, NULL);
        sw_ioctl(sc, "CAN CLEAR_MSG_FILTERS with none set", ch, CLEAR_MSG_FILTERS, NULL, NULL);
        sw_disconnect(sc, "CAN", ch);
    }
    if (sw_connect(sc, "ISO15765 Connect", ISO15765, 0, 500000, &ch) == 0) {
        sw_filter_cases(sc, "ISO15765", ch, ISO15765, SW_ISO_FILTERS, sizeof SW_ISO_FILTERS / sizeof SW_ISO_FILTERS[0]);
        sw_filter(sc, "ISO15765 FC 7E8 first", ch, FLOW_CONTROL_FILTER, hexmsg(&m, ISO15765, ISO15765_FRAME_PAD, "ffffffff"),
                  hexmsg(&p, ISO15765, ISO15765_FRAME_PAD, "000007e8"), hexmsg(&f, ISO15765, ISO15765_FRAME_PAD, "000007e0"), 1);
        sw_filter(sc, "ISO15765 FC 7E8 again (J2534 ERR_NOT_UNIQUE)", ch, FLOW_CONTROL_FILTER, &m, &p, &f, 1);
        sw_ioctl(sc, "ISO15765 CLEAR_MSG_FILTERS", ch, CLEAR_MSG_FILTERS, NULL, NULL);
        sw_disconnect(sc, "ISO15765", ch);
    }
    if (sw_connect(sc, "ISO14230 Connect", ISO14230, 0, 10400, &ch) == 0) {
        sw_filter_cases(sc, "ISO14230", ch, ISO14230, SW_KLINE_FILTERS, sizeof SW_KLINE_FILTERS / sizeof SW_KLINE_FILTERS[0]);
        sw_disconnect(sc, "ISO14230", ch);
    }
    sw_close(sc);
}

typedef struct { const char *name; J_U32 mproto, flags; const char *hex; } sw_wcase;

static const sw_wcase SW_CAN_WRITES[] = {
    { "11-bit, 8 data bytes", 0, 0, "000007df0201000000000000" },
    { "id only, DLC 0", 0, 0, "000007df" },
    { "29-bit with CAN_29BIT_ID", 0, CAN_29BIT_ID, "18db33f10201000000000000" },
    { "29-bit id without CAN_29BIT_ID", 0, 0, "18db33f10201000000000000" },
    { "11-bit id with CAN_29BIT_ID", 0, CAN_29BIT_ID, "000007df0201000000000000" },
    { "ISO15765_FRAME_PAD, 2 data bytes", 0, ISO15765_FRAME_PAD, "000007df0201" },
    { "SW_CAN_HV_TX", 0, SW_CAN_HV_TX, "000007df0201000000000000" },
    { "message ProtocolID ISO15765", ISO15765, 0, "000007df0201000000000000" },
};

static const sw_wcase SW_ISO_WRITES[] = {
    { "single frame, padded", 0, ISO15765_FRAME_PAD, "000007e03e00" },
    { "single frame, unpadded", 0, 0, "000007e03e00" },
    { "to 7E1, no flow-control filter for it", 0, ISO15765_FRAME_PAD, "000007e13e00" },
    { "19 data bytes, multi-frame", 0, ISO15765_FRAME_PAD, "000007e022f190f18cf187f189f191f192f193f194f1a0" },
    { "29-bit with CAN_29BIT_ID", 0, CAN_29BIT_ID | ISO15765_FRAME_PAD, "18da10f13e00" },
    { "extended addressing", 0, ISO15765_ADDR_TYPE | ISO15765_FRAME_PAD, "000006f1123e00" },
    { "message ProtocolID CAN", CAN, ISO15765_FRAME_PAD, "000007e03e00" },
};

static const sw_wcase SW_KLINE_WRITES[] = {
    { "KWP TesterPresent", 0, 0, "8110f13e" },
    { "WAIT_P3_MIN_ONLY", 0, WAIT_P3_MIN_ONLY, "8110f13e" },
    { "1 byte", 0, 0, "3e" },
};

static void sw_write_cases(const char *sc, const char *pn, J_U32 ch, J_U32 proto, const sw_wcase *cs, unsigned n)
{
    static PASSTHRU_MSG m;
    unsigned i; char name[128];
    for (i = 0; i < n; i++) {
        snprintf(name, sizeof name, "%s write %s", pn, cs[i].name);
        sw_write(sc, name, ch, hexmsg(&m, cs[i].mproto ? cs[i].mproto : proto, cs[i].flags, cs[i].hex), 1);
    }
}

static void sc_sweep_tx(void)
{
    const char *sc = "sweep_tx";
    static PASSTHRU_MSG m, p, f, batch[3], big;
    J_U32 ch; unsigned i; char name[96];
    SCONFIG lb1 = { LOOPBACK, 1 }, lb0 = { LOOPBACK, 0 };
    if (sw_open(sc) != 0) return;

    if (sw_connect(sc, "CAN Connect", CAN, 0, 500000, &ch) == 0) {
        sw_write_cases(sc, "CAN", ch, CAN, SW_CAN_WRITES, sizeof SW_CAN_WRITES / sizeof SW_CAN_WRITES[0]);
        for (i = 0; i < 3; i++) hexmsg(&batch[i], CAN, 0, "000007df0201000000000000");
        sw_write(sc, "CAN write three messages in one call", ch, batch, 3);
        sw_filter(sc, "CAN PASS all for the echo", ch, PASS_FILTER, hexmsg(&m, CAN, 0, "00000000"), hexmsg(&p, CAN, 0, "00000000"), NULL, 1);
        sw_config(sc, "CAN LOOPBACK set 1", ch, SET_CONFIG, &lb1, 1);
        sleep_ms(50);   /* let answers to the writes above arrive, so the clear takes them */
        sw_ioctl(sc, "CAN CLEAR_RX_BUFFER before the echo", ch, CLEAR_RX_BUFFER, NULL, NULL);
        sw_write(sc, "CAN write with LOOPBACK", ch, hexmsg(&m, CAN, 0, "000007df0201000000000000"), 1);
        mark(); read_some(sc, "CAN ReadMsgs the echo 200ms", ch, 8, 200);
        sw_config(sc, "CAN LOOPBACK set 0", ch, SET_CONFIG, &lb0, 1);
        sw_ioctl(sc, "CAN CLEAR_MSG_FILTERS", ch, CLEAR_MSG_FILTERS, NULL, NULL);

        hexmsg(&m, CAN, 0, "000007df0201000000000000");
        sw_periodic(sc, "CAN periodic every 5 ms", ch, &m, 5, 0);
        sw_periodic(sc, "CAN periodic every 65535 ms", ch, &m, 65535, 0);
        sw_periodic(sc, "CAN periodic 29-bit with CAN_29BIT_ID", ch, hexmsg(&p, CAN, CAN_29BIT_ID, "18db33f10201000000000000"), 1000, 0);
        sw_periodic(sc, "CAN periodic id only", ch, hexmsg(&p, CAN, 0, "000007df"), 1000, 0);
        for (i = 0; i < 11; i++) {
            snprintf(name, sizeof name, "CAN periodic %u of 11", i + 1);
            sw_periodic(sc, name, ch, &m, 1000, 1);
        }
        sw_ioctl(sc, "CAN CLEAR_PERIODIC_MSGS with 10 running", ch, CLEAR_PERIODIC_MSGS, NULL, NULL);
        sw_ioctl(sc, "CAN CLEAR_TX_BUFFER", ch, CLEAR_TX_BUFFER, NULL, NULL);
        sw_ioctl(sc, "CAN CLEAR_FUNCT_MSG_LOOKUP_TABLE", ch, CLEAR_FUNCT_MSG_LOOKUP_TABLE, NULL, NULL);
        sw_disconnect(sc, "CAN", ch);
    }

    if (sw_connect(sc, "ISO15765 Connect", ISO15765, 0, 500000, &ch) == 0) {
        sw_filter(sc, "ISO15765 FC 7E8/7E0", ch, FLOW_CONTROL_FILTER, hexmsg(&m, ISO15765, ISO15765_FRAME_PAD, "ffffffff"),
                  hexmsg(&p, ISO15765, ISO15765_FRAME_PAD, "000007e8"), hexmsg(&f, ISO15765, ISO15765_FRAME_PAD, "000007e0"), 1);
        sw_filter(sc, "ISO15765 FC 29-bit", ch, FLOW_CONTROL_FILTER, hexmsg(&m, ISO15765, CAN_29BIT_ID | ISO15765_FRAME_PAD, "1fffffff"),
                  hexmsg(&p, ISO15765, CAN_29BIT_ID | ISO15765_FRAME_PAD, "18daf110"), hexmsg(&f, ISO15765, CAN_29BIT_ID | ISO15765_FRAME_PAD, "18da10f1"), 1);
        sw_filter(sc, "ISO15765 FC extended addressing", ch, FLOW_CONTROL_FILTER,
                  hexmsg(&m, ISO15765, ISO15765_ADDR_TYPE | ISO15765_FRAME_PAD, "ffffffffff"),
                  hexmsg(&p, ISO15765, ISO15765_ADDR_TYPE | ISO15765_FRAME_PAD, "00000612f1"),
                  hexmsg(&f, ISO15765, ISO15765_ADDR_TYPE | ISO15765_FRAME_PAD, "000006f112"), 1);
        sw_write_cases(sc, "ISO15765", ch, ISO15765, SW_ISO_WRITES, sizeof SW_ISO_WRITES / sizeof SW_ISO_WRITES[0]);
        /* 4099 bytes is J2534's ceiling for normal addressing: the 4-byte id and 4095 data. */
        memset(&big, 0, sizeof big);
        big.ProtocolID = ISO15765; big.TxFlags = ISO15765_FRAME_PAD; big.DataSize = 4099;
        big.Data[2] = 0x07; big.Data[3] = 0xE0; big.Data[4] = 0x22;
        sw_write(sc, "ISO15765 write 4099 bytes", ch, &big, 1);
        sw_periodic(sc, "ISO15765 periodic single frame", ch, hexmsg(&p, ISO15765, ISO15765_FRAME_PAD, "000007e03e80"), 1000, 0);
        sw_periodic(sc, "ISO15765 periodic multi-frame", ch,
                    hexmsg(&p, ISO15765, ISO15765_FRAME_PAD, "000007e022f190f18cf187f189f191f192f193f194f1a0"), 1000, 0);
        sw_disconnect(sc, "ISO15765", ch);
    }

    if (sw_connect(sc, "ISO14230 Connect", ISO14230, 0, 10400, &ch) == 0) {
        SBYTE_ARRAY in, outb; unsigned char addr = 0x33, keys[8];
        sw_write_cases(sc, "ISO14230", ch, ISO14230, SW_KLINE_WRITES, sizeof SW_KLINE_WRITES / sizeof SW_KLINE_WRITES[0]);
        sw_periodic(sc, "ISO14230 periodic TesterPresent", ch, hexmsg(&p, ISO14230, 0, "8110f13e"), 2000, 0);
        /* FAST_INIT with a NULL pInput (J2534: the wake-up pattern with no
         * message) is not swept: the vendor DLL faults writing near address 0
         * and the process dies (1.02.0.4868, 2026-09-24). */
        in.NumOfBytes = 0; in.BytePtr = &addr; outb.NumOfBytes = sizeof keys; outb.BytePtr = keys;
        sw_ioctl(sc, "ISO14230 FIVE_BAUD_INIT zero bytes", ch, FIVE_BAUD_INIT, &in, &outb);
        in.NumOfBytes = 1;
        sw_ioctl(sc, "ISO14230 FIVE_BAUD_INIT 0x33", ch, FIVE_BAUD_INIT, &in, &outb);
        sw_disconnect(sc, "ISO14230", ch);
    }
    sw_close(sc);
}

/* Values J2534 does not allow. This driver refuses them before the wire; the
 * vendor DLL forwards them, so against a cable they are firmware input never
 * seen before. Kept out of the sweep so they run only on request and last:
 * if one wedges the firmware, the tap's final command names it and no reopen
 * follows (the vendor's reopen of a silent cable sends `tbi`). Rate 0 goes
 * last, as the likeliest to upset the CAN bit-timing arithmetic. */
static const sw_wcase SW_CAN_EDGE_WRITES[] = {
    { "3 bytes", 0, 0, "000007" },
    { "13 bytes", 0, 0, "000007df020100000000000000" },
    { "0 bytes", 0, 0, "-" },
};

static const sw_wcase SW_ISO_EDGE_WRITES[] = {
    { "ISO15765_ADDR_TYPE, id only", 0, ISO15765_ADDR_TYPE | ISO15765_FRAME_PAD, "000006f1" },
    { "3 bytes", 0, ISO15765_FRAME_PAD, "000007" },
};

static void sc_edge(void)
{
    const char *sc = "edge";
    static PASSTHRU_MSG m, p, f, big;
    J_U32 ch; unsigned i; char name[96], pat[16];
    if (sw_open(sc) != 0) return;

    if (sw_connect(sc, "CAN Connect", CAN, 0, 500000, &ch) == 0) {
        sw_filter(sc, "CAN filter type 0", ch, 0, hexmsg(&m, CAN, 0, "00000fff"), hexmsg(&p, CAN, 0, "000007e8"), NULL, 0);
        sw_filter(sc, "CAN filter type 4", ch, 4, hexmsg(&m, CAN, 0, "00000fff"), hexmsg(&p, CAN, 0, "000007e8"), NULL, 0);
        /* J2534-1 allows ten filters per channel; whose limit answers the eleventh? */
        for (i = 0; i < 11; i++) {
            snprintf(name, sizeof name, "CAN PASS filter %u of 11", i + 1);
            snprintf(pat, sizeof pat, "000007%02x", 0xE0 + i);
            sw_filter(sc, name, ch, PASS_FILTER, hexmsg(&m, CAN, 0, "00000fff"), hexmsg(&p, CAN, 0, pat), NULL, 1);
        }
        sw_ioctl(sc, "CAN CLEAR_MSG_FILTERS with 11 set", ch, CLEAR_MSG_FILTERS, NULL, NULL);
        sw_write_cases(sc, "CAN", ch, CAN, SW_CAN_EDGE_WRITES, sizeof SW_CAN_EDGE_WRITES / sizeof SW_CAN_EDGE_WRITES[0]);
        hexmsg(&m, CAN, 0, "000007df0201000000000000");
        sw_periodic(sc, "CAN periodic every 4 ms", ch, &m, 4, 0);
        sw_periodic(sc, "CAN periodic every 65536 ms", ch, &m, 65536, 0);
        sw_periodic(sc, "CAN periodic 3 bytes", ch, hexmsg(&p, CAN, 0, "000007"), 1000, 0);
        sw_disconnect(sc, "CAN", ch);
    }

    if (sw_connect(sc, "ISO15765 Connect", ISO15765, 0, 500000, &ch) == 0) {
        sw_filter(sc, "ISO15765 FC 7E8/7E0", ch, FLOW_CONTROL_FILTER, hexmsg(&m, ISO15765, ISO15765_FRAME_PAD, "ffffffff"),
                  hexmsg(&p, ISO15765, ISO15765_FRAME_PAD, "000007e8"), hexmsg(&f, ISO15765, ISO15765_FRAME_PAD, "000007e0"), 1);
        sw_write_cases(sc, "ISO15765", ch, ISO15765, SW_ISO_EDGE_WRITES, sizeof SW_ISO_EDGE_WRITES / sizeof SW_ISO_EDGE_WRITES[0]);
        memset(&big, 0, sizeof big);
        big.ProtocolID = ISO15765; big.TxFlags = ISO15765_FRAME_PAD; big.DataSize = 4100;
        big.Data[2] = 0x07; big.Data[3] = 0xE0; big.Data[4] = 0x22;
        sw_write(sc, "ISO15765 write 4100 bytes", ch, &big, 1);
        sw_disconnect(sc, "ISO15765", ch);
    }

    if (sw_connect(sc, "ISO14230 Connect", ISO14230, 0, 10400, &ch) == 0) {
        memset(&big, 0, sizeof big);
        big.ProtocolID = ISO14230; big.DataSize = 260; big.Data[0] = 0x80; big.Data[1] = 0x10; big.Data[2] = 0xF1; big.Data[3] = 0x3E;
        sw_write(sc, "ISO14230 write 260 bytes", ch, &big, 1);
        sw_disconnect(sc, "ISO14230", ch);
    }

    if (sw_connect(sc, "CAN rate 0", CAN, 0, 0, &ch) == 0)
        sw_disconnect(sc, "CAN rate 0", ch);
    sw_close(sc);
}

/* ---- main -------------------------------------------------------------- */
#define BIND(field, name) do { *(void **)(&api.field) = sym(name); if (!api.field) { fprintf(stderr, "missing export %s\n", name); return 2; } } while (0)

static const struct { const char *name; void (*fn)(void); int by_default; } SCENARIOS[] = {
    { "open", sc_open, 1 }, { "iso15765", sc_iso15765, 1 }, { "multiframe", sc_multiframe, 1 },
    { "timeouts", sc_timeouts, 1 }, { "errors", sc_errors, 1 }, { "periodic", sc_periodic, 1 },
    { "raw_can", sc_raw_can, 1 }, { "kline", sc_kline, 0 }, { "bench", sc_bench, 0 },
    { "sweep_connect", sc_sweep_connect, 0 }, { "sweep_config", sc_sweep_config, 0 },
    { "sweep_filter", sc_sweep_filter, 0 }, { "sweep_tx", sc_sweep_tx, 0 }, { "edge", sc_edge, 0 },
};

/* "sweep" selects every sweep_* scenario. */
static int selects(const char *sel, const char *name)
{
    size_t n = strlen(sel);
    return !strcmp(sel, "all") || !strcmp(sel, name) || (!strncmp(sel, name, n) && name[n] == '_');
}

int main(int argc, char **argv)
{
    const char *libpath, *tracepath, *replay_path = NULL;
    int i, repeat = 1, nsel = 0, r, wine = 0;
    const char *sel[16];

    if (argc < 3) { fprintf(stderr, "usage: %s <library> <trace.jsonl> [scenario ...] [--repeat N] [--cycles N]\n", argv[0]); return 2; }
    libpath = argv[1]; tracepath = argv[2];
    for (i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) bench_cycles = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_path = argv[++i];
        else if (nsel < 16) sel[nsel++] = argv[i];
    }
    out = strcmp(tracepath, "-") ? fopen(tracepath, "w") : stdout;
    if (!out) { perror(tracepath); return 2; }

#ifdef _WIN32
    if (wineshim_active()) { wine = 1; wineshim_install(stderr); }
#endif
    if (!load(libpath)) { fprintf(stderr, "cannot load %s\n", libpath); return 2; }
    BIND(open, "PassThruOpen"); BIND(close, "PassThruClose"); BIND(connect, "PassThruConnect");
    BIND(disconnect, "PassThruDisconnect"); BIND(read, "PassThruReadMsgs"); BIND(write, "PassThruWriteMsgs");
    BIND(startp, "PassThruStartPeriodicMsg"); BIND(stopp, "PassThruStopPeriodicMsg");
    BIND(filter, "PassThruStartMsgFilter"); BIND(stopf, "PassThruStopMsgFilter");
    BIND(progv, "PassThruSetProgrammingVoltage"); BIND(version, "PassThruReadVersion");
    BIND(lasterr, "PassThruGetLastError"); BIND(ioctl, "PassThruIoctl");

    fprintf(out, "{\"header\":true,\"library\":"); json_str(libpath);
    fprintf(out, ",\"platform\":\"%s\",\"wine\":%d,\"repeat\":%d}\n",
#ifdef _WIN32
            "win32",
#else
            "posix",
#endif
            wine, repeat);
    fflush(out);


    if (replay_path) {
        fprintf(out, "{\"begin\":\"replay\",\"file\":"); json_str(replay_path); fputs("}\n", out);
        replay(replay_path);
        fprintf(out, "{\"end\":true,\"calls\":%d}\n", seq);
        if (out != stdout) fclose(out);
        return 0;
    }
    for (r = 0; r < repeat; r++) {
        for (i = 0; i < (int)(sizeof SCENARIOS / sizeof SCENARIOS[0]); i++) {
            int run = nsel == 0 ? SCENARIOS[i].by_default : 0, k;
            for (k = 0; k < nsel; k++) if (selects(sel[k], SCENARIOS[i].name)) run = 1;
            if (!run) continue;
            fprintf(out, "{\"begin\":\"%s\",\"round\":%d}\n", SCENARIOS[i].name, r);
            SCENARIOS[i].fn();
        }
    }
    fprintf(out, "{\"end\":true,\"calls\":%d}\n", seq);
    if (out != stdout) fclose(out);
    return 0;
}
