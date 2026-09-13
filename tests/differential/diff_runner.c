/*
 * diff_runner — drive any J2534 library through one fixed sequence and print a
 * machine-diffable record of what it returned.
 *
 * Run it once against the reference library and once against this one; the two
 * outputs, plus the two wire traces from usbtap, are the differential result.
 *
 *   diff_runner <path-to-dylib> [--hardware]
 *
 * Without --hardware it runs only the steps that need no cable, so the
 * comparison still works on a machine with nothing attached.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/* dladdr and Dl_info are outside strict POSIX; ask for the Darwin set. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#include "j2534/j2534.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long (*fn_open)(const void *, J_U32 *);
typedef long (*fn_close)(J_U32);
typedef long (*fn_connect)(J_U32, J_U32, J_U32, J_U32, J_U32 *);
typedef long (*fn_disconnect)(J_U32);
typedef long (*fn_read)(J_U32, PASSTHRU_MSG *, J_U32 *, J_U32);
typedef long (*fn_write)(J_U32, const PASSTHRU_MSG *, J_U32 *, J_U32);
typedef long (*fn_startp)(J_U32, const PASSTHRU_MSG *, J_U32 *, J_U32);
typedef long (*fn_stopp)(J_U32, J_U32);
typedef long (*fn_filter)(J_U32, J_U32, const PASSTHRU_MSG *,
                          const PASSTHRU_MSG *, const PASSTHRU_MSG *, J_U32 *);
typedef long (*fn_stopf)(J_U32, J_U32);
typedef long (*fn_progv)(J_U32, J_U32, J_U32);
typedef long (*fn_version)(J_U32, char *, char *, char *);
typedef long (*fn_lasterr)(char *);
typedef long (*fn_ioctl)(J_U32, J_U32, const void *, void *);

static struct {
    fn_open open; fn_close close; fn_connect connect; fn_disconnect disconnect;
    fn_read read; fn_write write; fn_startp startp; fn_stopp stopp;
    fn_filter filter; fn_stopf stopf; fn_progv progv; fn_version version;
    fn_lasterr lasterr; fn_ioctl ioctl;
} api;

static void *lib;

#define BIND(field, name)                                              \
    do {                                                               \
        *(void **)(&api.field) = dlsym(lib, name);                     \
        if (api.field == NULL) {                                       \
            fprintf(stderr, "missing symbol %s\n", name);              \
            exit(2);                                                   \
        }                                                              \
    } while (0)

static void step(const char *what, long rc)
{
    printf("%-42s rc=%ld\n", what, rc);
}

int main(int argc, char **argv)
{
    J_U32 dev = 0, ch = 0, v = 0, fid = 0, msgid = 0;
    int hardware = 0;
    int i;

    /* Line-buffer: a library that hangs is killed by the harness watchdog,
     * and fully-buffered output would be lost with it — losing exactly the
     * record of which call wedged. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2) { fprintf(stderr, "usage: %s <dylib> [--hardware]\n", argv[0]); return 2; }
    for (i = 2; i < argc; i++)
        if (strcmp(argv[i], "--hardware") == 0) hardware = 1;

    lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

    BIND(open, "PassThruOpen");
    BIND(close, "PassThruClose");
    BIND(connect, "PassThruConnect");
    BIND(disconnect, "PassThruDisconnect");
    BIND(read, "PassThruReadMsgs");
    BIND(write, "PassThruWriteMsgs");
    BIND(startp, "PassThruStartPeriodicMsg");
    BIND(stopp, "PassThruStopPeriodicMsg");
    BIND(filter, "PassThruStartMsgFilter");
    BIND(stopf, "PassThruStopMsgFilter");
    BIND(progv, "PassThruSetProgrammingVoltage");
    BIND(version, "PassThruReadVersion");
    BIND(lasterr, "PassThruGetLastError");
    BIND(ioctl, "PassThruIoctl");

    /* Report the library dyld actually resolved, not the one we asked for.
     * DYLD_LIBRARY_PATH overrides even an absolute dlopen path by leaf name,
     * so "old" and "new" can silently become the same file — which produces a
     * differential result that agrees perfectly and means nothing. Print the
     * truth and refuse to continue if it is not what was requested. */
    {
        Dl_info info;
        const char *resolved = NULL;
        if (dladdr((const void *)(uintptr_t)api.open, &info) && info.dli_fname)
            resolved = info.dli_fname;
        printf("### requested: %s\n", argv[1]);
        printf("### resolved:  %s\n", resolved ? resolved : "<unknown>");
        if (resolved != NULL && strcmp(resolved, argv[1]) != 0) {
            fprintf(stderr,
                    "FATAL: dlopen(\"%s\") resolved to \"%s\".\n"
                    "Both libraries share the leaf name libj2534.dylib; clear "
                    "DYLD_LIBRARY_PATH so the absolute path is honoured.\n",
                    argv[1], resolved);
            return 4;
        }
    }

    /* --- calls that need no device ------------------------------------- */
    {
        char err[256];
        memset(err, 0, sizeof err);
        api.lasterr(err);
        printf("%-42s len=%zu\n", "GetLastError before open", strlen(err));
    }

    if (!hardware) {
        printf("### no-hardware section only\n");
        dlclose(lib);
        return 0;
    }

    step("PassThruOpen", api.open(NULL, &dev));

    {
        char fw[80] = "", dl[80] = "", ap[80] = "";
        step("PassThruReadVersion", api.version(dev, fw, dl, ap));
        printf("      firmware=%s api=%s\n", fw, ap);
    }

    step("Ioctl READ_VBATT", api.ioctl(dev, READ_VBATT, NULL, &v));
    printf("      vbatt=%lu\n", (unsigned long)v);

    step("PassThruConnect ISO15765 500000", api.connect(dev, ISO15765, 0, 500000, &ch));
    printf("      channel=%lu\n", (unsigned long)ch);

    {
        SCONFIG cfg[2];
        SCONFIG_LIST list;
        cfg[0].Parameter = ISO15765_BS;    cfg[0].Value = 0;
        cfg[1].Parameter = ISO15765_STMIN; cfg[1].Value = 0;
        list.NumOfParams = 2; list.ConfigPtr = cfg;
        step("Ioctl SET_CONFIG", api.ioctl(ch, SET_CONFIG, &list, NULL));
    }

    {
        PASSTHRU_MSG mask, pat, fc;
        memset(&mask, 0, sizeof mask); memset(&pat, 0, sizeof pat); memset(&fc, 0, sizeof fc);
        mask.ProtocolID = pat.ProtocolID = fc.ProtocolID = ISO15765;
        mask.DataSize = pat.DataSize = fc.DataSize = 4;
        memset(mask.Data, 0xFF, 4);
        pat.Data[2] = 0x07; pat.Data[3] = 0xE8;
        fc.Data[2]  = 0x07; fc.Data[3]  = 0xE0;
        step("StartMsgFilter FLOW_CONTROL",
             api.filter(ch, FLOW_CONTROL_FILTER, &mask, &pat, &fc, &fid));
        printf("      filter=%lu\n", (unsigned long)fid);
    }

    step("Ioctl CLEAR_RX_BUFFER", api.ioctl(ch, CLEAR_RX_BUFFER, NULL, NULL));

    {
        PASSTHRU_MSG m;
        J_U32 n = 1;
        memset(&m, 0, sizeof m);
        m.ProtocolID = ISO15765;
        m.TxFlags    = ISO15765_FRAME_PAD;
        m.DataSize   = 6;
        m.Data[2] = 0x07; m.Data[3] = 0xE0; m.Data[4] = 0x3E; m.Data[5] = 0x00;
        step("WriteMsgs $3E TesterPresent", api.write(ch, &m, &n, 1000));
        printf("      sent=%lu\n", (unsigned long)n);
    }

    {
        PASSTHRU_MSG in[4];
        J_U32 cnt = 4;
        memset(in, 0, sizeof in);
        step("ReadMsgs (500 ms)", api.read(ch, in, &cnt, 500));
        printf("      received=%lu\n", (unsigned long)cnt);
    }

    /* --- the behaviours the brief calls out as defects ------------------ */
    {
        PASSTHRU_MSG m;
        memset(&m, 0, sizeof m);
        m.ProtocolID = ISO15765; m.DataSize = 6;
        m.Data[2] = 0x07; m.Data[3] = 0xE0; m.Data[4] = 0x3E;
        step("StartPeriodicMsg (100 ms)", api.startp(ch, &m, &msgid, 100));
        printf("      msgid=%lu\n", (unsigned long)msgid);
        step("StopPeriodicMsg", api.stopp(ch, msgid));
    }
    step("SetProgrammingVoltage 12 @ 17000 mV", api.progv(dev, 12, 17000));

    /* --- error paths ---------------------------------------------------- */
    step("Connect twice (same protocol)", api.connect(dev, ISO15765, 0, 500000, &v));
    step("Disconnect bogus channel 9", api.disconnect(9));
    step("Ioctl unknown id 0xDEAD", api.ioctl(ch, 0xDEAD, NULL, NULL));
    {
        J_U32 f2 = 0;
        PASSTHRU_MSG pat, fc;
        memset(&pat, 0, sizeof pat); memset(&fc, 0, sizeof fc);
        pat.DataSize = 4; fc.DataSize = 4;
        step("StartMsgFilter NULL mask",
             api.filter(ch, FLOW_CONTROL_FILTER, NULL, &pat, &fc, &f2));
    }
    {
        char err[256];
        memset(err, 0, sizeof err);
        api.lasterr(err);
        printf("%-42s \"%s\"\n", "GetLastError after failure", err);
    }

    step("PassThruStopMsgFilter", api.stopf(ch, fid));
    step("PassThruDisconnect", api.disconnect(ch));
    step("PassThruClose", api.close(dev));

    dlclose(lib);
    return 0;
}
