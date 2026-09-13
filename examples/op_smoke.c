/*
 * op_smoke — exercise the library against a real cable.
 *
 * Bench-safe: it identifies the device, reads voltages, opens and closes an
 * ISO15765 channel and installs a filter. It transmits nothing onto the bus
 * unless --tx is given.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "j2534/j2534.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void report(const char *what, long rc, long want)
{
    char err[80] = "";
    PassThruGetLastError(err);
    if (rc == want) {
        printf("  ok    %-34s -> %ld\n", what, rc);
    } else {
        fails++;
        printf("  FAIL  %-34s -> %ld (want %ld) %s\n", what, rc, want, err);
    }
}

int main(int argc, char **argv)
{
    J_U32 dev = 0, ch = 0, vbatt = 0, prog = 0, fid = 0;
    char fw[80], dll[80], api[80];
    long rc;
    int do_tx = (argc > 1 && strcmp(argv[1], "--tx") == 0);

    printf("openport-j2534 smoke test\n");

    rc = PassThruOpen(NULL, &dev);
    report("PassThruOpen", rc, STATUS_NOERROR);
    if (rc != STATUS_NOERROR) {
        char err[80] = "";
        PassThruGetLastError(err);
        printf("\n  %s\n", err);
        return 1;
    }

    rc = PassThruReadVersion(dev, fw, dll, api);
    report("PassThruReadVersion", rc, STATUS_NOERROR);
    printf("        firmware=%s dll=%s api=%s\n", fw, dll, api);

    rc = PassThruIoctl(dev, READ_VBATT, NULL, &vbatt);
    report("Ioctl READ_VBATT", rc, STATUS_NOERROR);
    printf("        vbatt = %lu mV (%.2f V)\n", (unsigned long)vbatt, (double)vbatt / 1000.0);

    rc = PassThruIoctl(dev, READ_PROG_VOLTAGE, NULL, &prog);
    report("Ioctl READ_PROG_VOLTAGE", rc, STATUS_NOERROR);
    printf("        prog  = %lu mV\n", (unsigned long)prog);

    rc = PassThruConnect(dev, ISO15765, 0, 500000, &ch);
    report("PassThruConnect ISO15765 500k", rc, STATUS_NOERROR);

    /* Opening the same protocol twice must be refused by channel, not ignored. */
    {
        J_U32 dup = 0;
        rc = PassThruConnect(dev, ISO15765, 0, 500000, &dup);
        report("Connect twice -> ERR_CHANNEL_IN_USE", rc, ERR_CHANNEL_IN_USE);
    }

    {
        SCONFIG cfg[2];
        SCONFIG_LIST list;
        cfg[0].Parameter = ISO15765_BS;    cfg[0].Value = 0;
        cfg[1].Parameter = ISO15765_STMIN; cfg[1].Value = 0;
        list.NumOfParams = 2; list.ConfigPtr = cfg;
        rc = PassThruIoctl(ch, SET_CONFIG, &list, NULL);
        report("Ioctl SET_CONFIG BS/STMIN", rc, STATUS_NOERROR);

        cfg[0].Parameter = DATA_RATE;
        list.NumOfParams = 1;
        rc = PassThruIoctl(ch, GET_CONFIG, &list, NULL);
        report("Ioctl GET_CONFIG DATA_RATE", rc, STATUS_NOERROR);
        printf("        data rate = %lu\n", (unsigned long)cfg[0].Value);
    }

    {
        PASSTHRU_MSG mask, pat, fc;
        memset(&mask, 0, sizeof mask); memset(&pat, 0, sizeof pat);
        memset(&fc, 0, sizeof fc);
        mask.ProtocolID = pat.ProtocolID = fc.ProtocolID = ISO15765;
        mask.DataSize = pat.DataSize = fc.DataSize = 4;
        mask.Data[0]=0xFF; mask.Data[1]=0xFF; mask.Data[2]=0xFF; mask.Data[3]=0xFF;
        pat.Data[2]=0x07; pat.Data[3]=0xE8;
        fc.Data[2]=0x07;  fc.Data[3]=0xE0;
        rc = PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, &pat, &fc, &fid);
        report("StartMsgFilter FLOW_CONTROL", rc, STATUS_NOERROR);
        printf("        filter id = %lu\n", (unsigned long)fid);
    }

    rc = PassThruIoctl(ch, CLEAR_RX_BUFFER, NULL, NULL);
    report("Ioctl CLEAR_RX_BUFFER", rc, STATUS_NOERROR);

    /* Refused unless the caller opted in. Nothing reaches the pin. */
    rc = PassThruSetProgrammingVoltage(dev, 12, 17000);
    report("SetProgrammingVoltage (gated)", rc, ERR_NOT_SUPPORTED);

    rc = PassThruIoctl(ch, 0xDEAD, NULL, NULL);
    report("Ioctl bad id -> ERR_INVALID_IOCTL_ID", rc, ERR_INVALID_IOCTL_ID);

    if (do_tx) {
        PASSTHRU_MSG m;
        J_U32 n = 1;
        memset(&m, 0, sizeof m);
        m.ProtocolID = ISO15765;
        m.TxFlags    = ISO15765_FRAME_PAD;
        m.DataSize   = 6;
        m.Data[2]=0x07; m.Data[3]=0xE0; m.Data[4]=0x3E; m.Data[5]=0x00;
        rc = PassThruWriteMsgs(ch, &m, &n, 1000);
        printf("  info  WriteMsgs $3E                -> %ld (%lu sent)\n",
               rc, (unsigned long)n);
        {
            PASSTHRU_MSG in[4];
            J_U32 cnt = 4;
            rc = PassThruReadMsgs(ch, in, &cnt, 500);
            printf("  info  ReadMsgs                     -> %ld (%lu msgs)\n",
                   rc, (unsigned long)cnt);
        }
    }

    rc = PassThruStopMsgFilter(ch, fid);
    report("PassThruStopMsgFilter", rc, STATUS_NOERROR);
    rc = PassThruDisconnect(ch);
    report("PassThruDisconnect", rc, STATUS_NOERROR);
    rc = PassThruClose(dev);
    report("PassThruClose", rc, STATUS_NOERROR);

    printf("\n%s\n", fails == 0 ? "all checks passed" : "FAILURES PRESENT");
    return fails == 0 ? 0 : 1;
}
