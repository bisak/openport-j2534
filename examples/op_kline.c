/*
 * op_kline — the driver's K-line path against a real ECU, end to end.
 *
 * Read-only: one StartCommunication init (fast or five-baud), one
 * ReadEcuIdentification, one TesterPresent. Nothing else is ever sent.
 *
 *   op_kline [--five] [--eobd]
 *     --five   five-baud init (address byte) instead of fast init
 *     --eobd   functional address 0x33 instead of the VAG engine address 0x01
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "j2534/j2534.h"

#include <stdio.h>
#include <string.h>

static void hex(const unsigned char *d, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) printf("%02x ", d[i]);
    printf("\n");
}

static void fail(const char *what, long rc)
{
    char err[80] = "";
    PassThruGetLastError(err);
    printf("  FAIL  %s -> %ld %s\n", what, rc, err);
}

int main(int argc, char **argv)
{
    J_U32 dev = 0, ch = 0, n, fid = 0;
    int five = 0, eobd = 0, i;
    long rc;
    unsigned char target;
    PASSTHRU_MSG mask, pat, req, rsp, in[4];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--five") == 0) five = 1;
        if (strcmp(argv[i], "--eobd") == 0) eobd = 1;
    }
    target = eobd ? 0x33 : 0x01;

    rc = PassThruOpen(NULL, &dev);
    if (rc) { fail("PassThruOpen", rc); return 1; }

    rc = PassThruConnect(dev, ISO14230, 0, 10400, &ch);
    if (rc) { fail("Connect ISO14230 @ 10400", rc); PassThruClose(dev); return 1; }
    printf("  ok    Connect ISO14230 -> channel %lu\n", (unsigned long)ch);

    /* Pass-all filter: reported necessary for K-line receive. */
    memset(&mask, 0, sizeof mask); mask.ProtocolID = ISO14230; mask.DataSize = 1;
    memset(&pat, 0, sizeof pat);   pat.ProtocolID = ISO14230;  pat.DataSize = 1;
    rc = PassThruStartMsgFilter(ch, PASS_FILTER, &mask, &pat, NULL, &fid);
    if (rc) fail("PASS_FILTER", rc); else printf("  ok    PASS_FILTER -> id %lu\n", (unsigned long)fid);

    if (five) {
        SBYTE_ARRAY sin, sout;
        unsigned char keys[8];
        sin.NumOfBytes = 1; sin.BytePtr = &target;
        sout.NumOfBytes = sizeof keys; sout.BytePtr = keys;
        rc = PassThruIoctl(ch, FIVE_BAUD_INIT, &sin, &sout);
        if (rc) fail("FIVE_BAUD_INIT", rc);
        else { printf("  ok    FIVE_BAUD_INIT addr 0x%02x -> %lu keybyte(s): ", target,
                      (unsigned long)sout.NumOfBytes); hex(keys, (unsigned)sout.NumOfBytes); }
    } else {
        memset(&req, 0, sizeof req);
        req.ProtocolID = ISO14230; req.DataSize = 4;
        req.Data[0] = (unsigned char)(eobd ? 0xC1 : 0x81);
        req.Data[1] = target; req.Data[2] = 0xF1; req.Data[3] = 0x81;
        rc = PassThruIoctl(ch, FAST_INIT, &req, &rsp);
        if (rc) fail("FAST_INIT", rc);
        else { printf("  ok    FAST_INIT StartCommunication -> %lu byte(s): ",
                      (unsigned long)rsp.DataSize); hex(rsp.Data, (unsigned)rsp.DataSize); }
    }

    /* ReadEcuIdentification 0x9B, then TesterPresent; both are reads. */
    {
        const unsigned char reqs[2][5] = {
            { (unsigned char)((eobd ? 0xC0 : 0x80) | 2), target, 0xF1, 0x1A, 0x9B },
            { (unsigned char)((eobd ? 0xC0 : 0x80) | 1), target, 0xF1, 0x3E, 0x00 } };
        const unsigned lens[2] = { 5, 4 };
        int k;
        for (k = 0; k < 2; k++) {
            memset(&req, 0, sizeof req);
            req.ProtocolID = ISO14230; req.DataSize = lens[k];
            memcpy(req.Data, reqs[k], lens[k]);
            n = 1;
            rc = PassThruWriteMsgs(ch, &req, &n, 2000);
            printf("  %s  WriteMsgs ", rc ? "FAIL" : "ok  "); hex(req.Data, (unsigned)req.DataSize);
            if (rc) { fail("WriteMsgs", rc); continue; }
            n = 4;
            rc = PassThruReadMsgs(ch, in, &n, 3000);
            printf("  %s  ReadMsgs -> rc %ld, %lu message(s)\n", rc ? "FAIL" : "ok  ", rc,
                   (unsigned long)n);
            for (i = 0; i < (int)n; i++) {
                printf("        RxStatus=0x%02lx ts=%lu DataSize=%lu data=",
                       (unsigned long)in[i].RxStatus, (unsigned long)in[i].Timestamp,
                       (unsigned long)in[i].DataSize);
                hex(in[i].Data, (unsigned)in[i].DataSize);
            }
        }
    }

    PassThruDisconnect(ch);
    PassThruClose(dev);
    return 0;
}
