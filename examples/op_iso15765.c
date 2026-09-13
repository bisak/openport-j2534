/*
 * op_iso15765 — the driver's ISO15765 and raw-CAN receive paths against a real
 * ECU, through the shipped C ABI rather than a script's own parser.
 *
 * Read-only: legislated OBD mode 01 and mode 09 only.
 *
 * It also demonstrates ISO 15765-4 clause 8.1 on live hardware: the same
 * request is sent with and without ISO15765_FRAME_PAD, and a conforming ECU
 * answers only the padded one.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "j2534/j2534.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void hex(const unsigned char *d, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) printf("%02x ", d[i]);
}

static void fail(const char *what, long rc)
{
    char err[80] = "";
    PassThruGetLastError(err);
    fails++;
    printf("  FAIL  %s -> %ld %s\n", what, rc, err);
}

static PASSTHRU_MSG mk(J_U32 proto, J_U32 cid, const unsigned char *pl,
                       unsigned n, J_U32 flags)
{
    PASSTHRU_MSG m;
    unsigned i;
    memset(&m, 0, sizeof m);
    m.ProtocolID = proto;
    m.TxFlags = flags;
    m.Data[0] = (unsigned char)(cid >> 24); m.Data[1] = (unsigned char)(cid >> 16);
    m.Data[2] = (unsigned char)(cid >> 8);  m.Data[3] = (unsigned char)cid;
    for (i = 0; i < n; i++) m.Data[4 + i] = pl[i];
    m.DataSize = 4 + n;
    return m;
}

/* Print every message the driver hands back, then return how many were real
 * received data (neither our own echo nor an indication). */
static int drain_print(J_U32 ch, const char *label, J_U32 timeout)
{
    PASSTHRU_MSG in[8];
    J_U32 n = 8;
    long rc = PassThruReadMsgs(ch, in, &n, timeout);
    int data = 0, i;
    printf("  %-46s rc=%ld  %lu message(s)\n", label, rc, (unsigned long)n);
    for (i = 0; i < (int)n; i++) {
        printf("        RxStatus=0x%08lx DataSize=%-4lu ExtraDataIndex=%-4lu ts=%-10lu ",
               (unsigned long)in[i].RxStatus, (unsigned long)in[i].DataSize,
               (unsigned long)in[i].ExtraDataIndex, (unsigned long)in[i].Timestamp);
        hex(in[i].Data, (unsigned)in[i].DataSize);
        if (in[i].RxStatus & TX_MSG_TYPE)          printf(" [own transmit]");
        else if (in[i].RxStatus & ISO15765_FIRST_FRAME) printf(" [first-frame indication]");
        else { printf(" [received data]"); data++; }
        printf("\n");
    }
    return data;
}

int main(void)
{
    J_U32 dev = 0, ch = 0, fid = 0, n;
    long rc;
    PASSTHRU_MSG mask, pat, flow, m;
    SCONFIG cfg[2];
    SCONFIG_LIST list;
    static const unsigned char pid00[] = { 0x01, 0x00 };
    static const unsigned char vin[]   = { 0x09, 0x02 };

    rc = PassThruOpen(NULL, &dev);
    if (rc) { fail("PassThruOpen", rc); return 1; }

    /* ---- ISO15765 ---------------------------------------------------- */
    rc = PassThruConnect(dev, ISO15765, 0, 500000, &ch);
    if (rc) { fail("Connect ISO15765", rc); PassThruClose(dev); return 1; }
    printf("  ok    Connect ISO15765 @ 500k -> channel %lu\n", (unsigned long)ch);

    cfg[0].Parameter = ISO15765_BS;    cfg[0].Value = 0;
    cfg[1].Parameter = ISO15765_STMIN; cfg[1].Value = 0;
    list.NumOfParams = 2; list.ConfigPtr = cfg;
    rc = PassThruIoctl(ch, SET_CONFIG, &list, NULL);
    printf("  %s  SET_CONFIG BS/STMIN -> %ld\n", rc ? "FAIL" : "ok  ", rc);

    mask = mk(ISO15765, 0xFFFFFFFF, NULL, 0, ISO15765_FRAME_PAD);
    pat  = mk(ISO15765, 0x7E8,      NULL, 0, ISO15765_FRAME_PAD);
    flow = mk(ISO15765, 0x7E0,      NULL, 0, ISO15765_FRAME_PAD);
    rc = PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, &pat, &flow, &fid);
    if (rc) fail("FLOW_CONTROL_FILTER", rc);
    else printf("  ok    FLOW_CONTROL_FILTER -> id %lu\n", (unsigned long)fid);

    /* ISO 15765-4 clause 8.1: unpadded must be ignored by a conforming ECU. */
    PassThruIoctl(ch, CLEAR_RX_BUFFER, NULL, NULL);
    m = mk(ISO15765, 0x7E0, pid00, sizeof pid00, 0);
    n = 1;
    rc = PassThruWriteMsgs(ch, &m, &n, 1000);
    printf("\n  -- mode 01 PID 00, NO padding (ISO 15765-4 says ignore) --\n");
    printf("  %s  WriteMsgs -> %ld\n", rc ? "FAIL" : "ok  ", rc);
    if (drain_print(ch, "ReadMsgs (expect no received data)", 1000) != 0)
        printf("        NOTE: this ECU answered an unpadded request\n");

    PassThruIoctl(ch, CLEAR_RX_BUFFER, NULL, NULL);
    m = mk(ISO15765, 0x7E0, pid00, sizeof pid00, ISO15765_FRAME_PAD);
    n = 1;
    rc = PassThruWriteMsgs(ch, &m, &n, 1000);
    printf("\n  -- mode 01 PID 00, padded (single frame) --\n");
    printf("  %s  WriteMsgs -> %ld\n", rc ? "FAIL" : "ok  ", rc);
    if (drain_print(ch, "ReadMsgs (expect one received message)", 1500) < 1)
        fails++;

    PassThruIoctl(ch, CLEAR_RX_BUFFER, NULL, NULL);
    m = mk(ISO15765, 0x7E0, vin, sizeof vin, ISO15765_FRAME_PAD);
    n = 1;
    rc = PassThruWriteMsgs(ch, &m, &n, 1000);
    printf("\n  -- mode 09 PID 02 VIN, padded (multi-frame reassembly) --\n");
    printf("  %s  WriteMsgs -> %ld\n", rc ? "FAIL" : "ok  ", rc);
    if (drain_print(ch, "ReadMsgs (expect indication + reassembled)", 2000) < 1)
        fails++;

    PassThruStopMsgFilter(ch, fid);
    PassThruDisconnect(ch);

    /* ---- raw CAN, the other receive path through the driver ----------- */
    rc = PassThruConnect(dev, CAN, 0, 500000, &ch);
    if (rc) fail("Connect CAN", rc);
    else {
        static const unsigned char raw[] = { 0x02, 0x01, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55 };
        printf("\n  ok    Connect raw CAN @ 500k -> channel %lu\n", (unsigned long)ch);
        mask = mk(CAN, 0x00000000, NULL, 0, 0);
        pat  = mk(CAN, 0x00000000, NULL, 0, 0);
        rc = PassThruStartMsgFilter(ch, PASS_FILTER, &mask, &pat, NULL, &fid);
        printf("  %s  PASS_FILTER (accept all) -> %ld\n", rc ? "FAIL" : "ok  ", rc);
        m = mk(CAN, 0x7DF, raw, sizeof raw, 0);
        n = 1;
        rc = PassThruWriteMsgs(ch, &m, &n, 1000);
        printf("  %s  WriteMsgs raw 8-byte OBD frame -> %ld\n", rc ? "FAIL" : "ok  ", rc);
        printf("\n  -- raw CAN receive through the driver --\n");
        drain_print(ch, "ReadMsgs", 1500);
        PassThruDisconnect(ch);
    }

    PassThruClose(dev);
    printf("\n  %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
