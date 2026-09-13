/*
 * API-level tests through the mock transport: no cable, no libusb device.
 * These cover the defect list in the project brief — every one of them is a
 * behaviour a caller can depend on.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"
#include "j2534/j2534.h"
#include "op_device.h"
#include "mock_transport.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static J_U32 open_device(void)
{
    J_U32 dev = 0;
    mock_reset();
    mock_install_openport_responder();
    op_device_set_factory(mock_open);
    CHECK_EQ(PassThruOpen(NULL, &dev), STATUS_NOERROR, "PassThruOpen");
    return dev;
}

static void shut(J_U32 dev) { PassThruClose(dev); op_device_set_factory(NULL); }

static int tx_contains(const char *needle)
{
    size_t n = 0;
    const uint8_t *tx = mock_tx(&n);
    size_t need = strlen(needle);
    size_t i;
    if (need > n) return 0;
    for (i = 0; i + need <= n; i++)
        if (memcmp(tx + i, needle, need) == 0) return 1;
    return 0;
}

static void null_parameters(void)
{
    J_U32 dev, ch = 0, n = 1;
    PASSTHRU_MSG m;

    /* Before any device exists, every entry point must refuse cleanly. */
    op_device_set_factory(mock_open);
    mock_reset();
    CHECK_EQ(PassThruOpen(NULL, NULL), ERR_NULL_PARAMETER, "Open(NULL id)");

    dev = open_device();

    CHECK_EQ(PassThruConnect(dev, ISO15765, 0, 500000, NULL),
             ERR_NULL_PARAMETER, "Connect(NULL channel)");
    CHECK_EQ(PassThruReadMsgs(6, &m, NULL, 10),
             ERR_NULL_PARAMETER, "ReadMsgs(NULL count)");
    CHECK_EQ(PassThruWriteMsgs(6, NULL, &n, 10),
             ERR_NULL_PARAMETER, "WriteMsgs(NULL msg)");
    CHECK_EQ(PassThruGetLastError(NULL), ERR_NULL_PARAMETER,
             "GetLastError(NULL)");

    /* The old driver dereferenced mask/pattern before checking them; a caller
     * passing NULL got a crash instead of a return code. */
    CHECK_EQ(PassThruConnect(dev, ISO15765, 0, 500000, &ch), STATUS_NOERROR,
             "Connect for filter tests");
    {
        J_U32 fid = 0;
        PASSTHRU_MSG mask, pat, fc;
        memset(&mask, 0, sizeof mask); mask.DataSize = 4;
        memset(&pat,  0, sizeof pat);  pat.DataSize  = 4;
        memset(&fc,   0, sizeof fc);   fc.DataSize   = 4;

        CHECK_EQ(PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, NULL, &pat, &fc, &fid),
                 ERR_NULL_PARAMETER, "StartMsgFilter(NULL mask)");
        CHECK_EQ(PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, NULL, &fc, &fid),
                 ERR_NULL_PARAMETER, "StartMsgFilter(NULL pattern)");
        CHECK_EQ(PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, &pat, NULL, &fid),
                 ERR_NULL_PARAMETER, "StartMsgFilter(NULL flow control)");
        CHECK_EQ(PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, &pat, &fc, NULL),
                 ERR_NULL_PARAMETER, "StartMsgFilter(NULL id)");
    }
    shut(dev);
}

static void connect_and_channels(void)
{
    J_U32 dev = open_device(), ch = 0, ch2 = 0;

    CHECK_EQ(PassThruConnect(dev, ISO15765, 0, 500000, &ch), STATUS_NOERROR,
             "Connect ISO15765");
    CHECK_EQ(ch, ISO15765, "channel id is the protocol id");
    CHECK(tx_contains("ato6 0 500000 0 "), "Connect emits ato6");

    CHECK_EQ(PassThruConnect(dev, ISO15765, 0, 500000, &ch2), ERR_CHANNEL_IN_USE,
             "second Connect on the same protocol");

    CHECK_EQ(PassThruConnect(dev, 99, 0, 500000, &ch2), ERR_INVALID_PROTOCOL_ID,
             "Connect with a bad protocol");
    CHECK_EQ(PassThruConnect(dev, ISO9141, 0, 0, &ch2), ERR_INVALID_BAUDRATE,
             "Connect with zero baud");

    CHECK_EQ(PassThruDisconnect(ch), STATUS_NOERROR, "Disconnect");
    CHECK(tx_contains("atc6 "), "Disconnect emits atc6");
    CHECK_EQ(PassThruDisconnect(ch), ERR_INVALID_CHANNEL_ID,
             "Disconnect twice");
    CHECK_EQ(PassThruReadMsgs(ch, NULL, NULL, 0), ERR_NULL_PARAMETER,
             "ReadMsgs on a closed channel");
    shut(dev);
}

/* Defect 1: these returned STATUS_NOERROR while doing nothing. A caller using
 * StartPeriodicMsg as a TesterPresent keep-alive would have got silence. */
static void periodic_is_real(void)
{
    J_U32 dev = open_device(), ch = 0, id = 0;
    PASSTHRU_MSG m;

    PassThruConnect(dev, ISO15765, 0, 500000, &ch);
    memset(&m, 0, sizeof m);
    m.ProtocolID = ISO15765;
    m.DataSize   = 6;
    m.Data[3] = 0xE0; m.Data[4] = 0x3E; m.Data[5] = 0x00;

    CHECK_EQ(PassThruStartPeriodicMsg(ch, &m, &id, 0),
             ERR_INVALID_TIME_INTERVAL, "interval below the J2534 minimum");
    CHECK_EQ(PassThruStartPeriodicMsg(ch, &m, &id, 70000),
             ERR_INVALID_TIME_INTERVAL, "interval above the J2534 maximum");

    CHECK_EQ(PassThruStartPeriodicMsg(ch, &m, &id, 10), STATUS_NOERROR,
             "StartPeriodicMsg accepted");
    CHECK(id != 0, "StartPeriodicMsg returns a usable id");

    mock_clear_tx();
    usleep(120000);              /* ~12 intervals */
    CHECK(tx_contains("att6 6 "), "periodic message actually transmits");

    CHECK_EQ(PassThruStopPeriodicMsg(ch, id + 100), ERR_INVALID_MSG_ID,
             "StopPeriodicMsg with a bad id");
    CHECK_EQ(PassThruStopPeriodicMsg(ch, id), STATUS_NOERROR, "StopPeriodicMsg");

    mock_clear_tx();
    usleep(80000);
    CHECK(!tx_contains("att6 6 "), "stopped periodic message stays stopped");
    shut(dev);
}

/*
 * Stopping a periodic message must mean nothing more goes on the bus. The
 * scheduler releases the periodic lock before transmitting — it has to, or it
 * would take the device command lock in the opposite order to everyone else —
 * so a frame can already be on the wire when the stop arrives. Returning then
 * would tell a caller the bus is quiet while a frame is still going out, and
 * the vehicle tooling's "nothing transmits after Stop" guarantee rests on this.
 * With a slow transmit the race is deterministic rather than a rare flake.
 */
static void periodic_stop_waits_for_inflight(void)
{
    J_U32 dev = open_device(), ch = 0, id = 0;
    PASSTHRU_MSG m;
    struct timeval t0, t1;
    long ms;

    PassThruConnect(dev, ISO15765, 0, 500000, &ch);
    memset(&m, 0, sizeof m);
    m.ProtocolID = ISO15765;
    m.DataSize   = 6;
    m.Data[3] = 0xE0; m.Data[4] = 0x3E; m.Data[5] = 0x00;

    mock_delay_transmits(250);
    CHECK_EQ(PassThruStartPeriodicMsg(ch, &m, &id, 10), STATUS_NOERROR,
             "periodic started");
    usleep(60000);                       /* a transmit is now on the wire */

    gettimeofday(&t0, NULL);
    CHECK_EQ(PassThruStopPeriodicMsg(ch, id), STATUS_NOERROR, "StopPeriodicMsg");
    gettimeofday(&t1, NULL);
    ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000L;
    CHECK(ms >= 50, "Stop waits out the frame already on the wire");

    mock_clear_tx();
    usleep(150000);
    CHECK(!tx_contains("att6 6 "), "and nothing transmits after Stop returns");
    mock_delay_transmits(0);
    shut(dev);
}

/* Defect 1 again: a stub must say so, and must not energise a pin by accident. */
static void programming_voltage(void)
{
    J_U32 dev = open_device();

    unsetenv("OPENPORT_ENABLE_PROG_VOLTAGE");
    CHECK_EQ(PassThruSetProgrammingVoltage(dev, 12, 17000), ERR_NOT_SUPPORTED,
             "applying voltage is refused unless enabled");
    CHECK(!tx_contains("atv 12 17000"), "refused voltage never reaches the wire");

    /* Turning it off is always permitted: a caller must be able to make the
     * pin safe without first opting in to making it live. */
    mock_clear_tx();
    CHECK_EQ(PassThruSetProgrammingVoltage(dev, 12, 0xFFFFFFFFUL),
             STATUS_NOERROR, "VOLTAGE_OFF is always allowed");
    CHECK(tx_contains("atv 12 4294967295 "), "VOLTAGE_OFF reaches the wire");

    setenv("OPENPORT_ENABLE_PROG_VOLTAGE", "1", 1);
    mock_clear_tx();
    CHECK_EQ(PassThruSetProgrammingVoltage(dev, 12, 17000), STATUS_NOERROR,
             "enabled voltage is applied");
    CHECK(tx_contains("atv 12 17000 "), "enabled voltage reaches the wire");
    unsetenv("OPENPORT_ENABLE_PROG_VOLTAGE");
    shut(dev);
}

/* Defect 5: GetLastError returned an empty string, so a caller could not
 * report why anything failed. */
static void last_error_is_useful(void)
{
    J_U32 dev = open_device();
    char buf[80];

    PassThruDisconnect(9);                     /* guaranteed to fail */
    memset(buf, 0, sizeof buf);
    CHECK_EQ(PassThruGetLastError(buf), STATUS_NOERROR, "GetLastError returns ok");
    CHECK(strlen(buf) > 0, "GetLastError describes the failure");
    CHECK(strstr(buf, "ERR_INVALID_CHANNEL_ID") != NULL,
          "GetLastError names the code, got \"%s\"", buf);
    shut(dev);
}

/* Defect 3: a wedged cable must produce ERR_TIMEOUT, never a hang. */
static void timeouts_are_honoured(void)
{
    J_U32 dev, ch = 0, n = 1;
    PASSTHRU_MSG m;

    dev = open_device();
    PassThruConnect(dev, ISO15765, 0, 500000, &ch);

    /* A silent device: the responder is removed, so nothing ever replies. */
    mock_set_responder(NULL);
    memset(&m, 0, sizeof m);
    m.DataSize = 6;
    n = 1;
    CHECK_EQ(PassThruWriteMsgs(ch, &m, &n, 100), ERR_TIMEOUT,
             "write to a silent device times out");
    CHECK_EQ(n, 0, "no messages reported as written");

    mock_install_openport_responder();

    /* Reading with nothing queued returns promptly and says so. */
    {
        PASSTHRU_MSG got[2];
        J_U32 count = 2;
        CHECK_EQ(PassThruReadMsgs(ch, got, &count, 50), ERR_BUFFER_EMPTY,
                 "empty read reports ERR_BUFFER_EMPTY");
        CHECK_EQ(count, 0, "empty read reports zero messages");
    }
    shut(dev);
}

/* Defect 2: transport failures were ignored and success returned regardless. */
static void transport_failures_propagate(void)
{
    J_U32 dev = open_device(), ch = 0, n = 1;
    PASSTHRU_MSG m;

    PassThruConnect(dev, ISO15765, 0, 500000, &ch);
    memset(&m, 0, sizeof m);
    m.DataSize = 6;

    mock_fail_writes(1, OP_ERR_IO);
    n = 1;
    CHECK_EQ(PassThruWriteMsgs(ch, &m, &n, 100), ERR_FAILED,
             "a failed USB write is reported");

    mock_fail_writes(1, OP_ERR_NO_DEVICE);
    n = 1;
    CHECK_EQ(PassThruWriteMsgs(ch, &m, &n, 100), ERR_DEVICE_NOT_CONNECTED,
             "an unplugged cable is reported");
    shut(dev);
}

static void device_absent(void)
{
    J_U32 dev = 0;
    /* No device: PassThruOpen must return 8, and later calls must not crash. */
    op_device_set_factory(NULL);
    mock_reset();
    CHECK_EQ(PassThruClose(1), ERR_INVALID_DEVICE_ID, "Close with no device: invalid id, as the standard and the vendor DLL say");
    CHECK_EQ(PassThruConnect(1, ISO15765, 0, 500000, &dev),
             ERR_DEVICE_NOT_CONNECTED, "Connect with no device");
    CHECK_EQ(PassThruIoctl(1, READ_VBATT, NULL, &dev),
             ERR_DEVICE_NOT_CONNECTED, "Ioctl with no device");
}

static void filters(void)
{
    J_U32 dev = open_device(), ch = 0, fid = 99;
    PASSTHRU_MSG mask, pat, fc;

    PassThruConnect(dev, ISO15765, 0, 500000, &ch);

    memset(&mask, 0, sizeof mask); mask.DataSize = 4;
    memset(&pat,  0, sizeof pat);  pat.DataSize  = 4;
    memset(&fc,   0, sizeof fc);   fc.DataSize   = 4;
    mask.Data[0]=0xFF; mask.Data[1]=0xFF; mask.Data[2]=0xFF; mask.Data[3]=0xFF;
    pat.Data[2]=0x07;  pat.Data[3]=0xE8;
    fc.Data[2]=0x07;   fc.Data[3]=0xE0;

    mock_clear_tx();
    CHECK_EQ(PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, &pat, &fc, &fid),
             STATUS_NOERROR, "flow-control filter accepted");
    CHECK_EQ(fid, 0, "filter id from the device");
    /* The device takes ONE per-message length, then that many messages. */
    CHECK(tx_contains("atf6 3 0 4 "), "filter command encoding");

    /* Mismatched lengths would silently shift the pattern on the wire. */
    pat.DataSize = 5;
    CHECK_EQ(PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, &pat, &fc, &fid),
             ERR_INVALID_MSG, "mismatched filter message lengths rejected");
    pat.DataSize = 4;

    mask.DataSize = 0;
    CHECK_EQ(PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, &pat, &fc, &fid),
             ERR_INVALID_MSG, "zero-length filter rejected");
    mask.DataSize = 4;

    CHECK_EQ(PassThruStartMsgFilter(ch, 42, &mask, &pat, &fc, &fid),
             ERR_INVALID_MSG, "unknown filter type rejected");

    /* DEC2004 7.2.9.2: flow control on a PASS/BLOCK filter is an error.
     * Silently ignoring it would let a caller think it had been applied. */
    CHECK_EQ(PassThruStartMsgFilter(ch, PASS_FILTER, &mask, &pat, &fc, &fid),
             ERR_INVALID_MSG, "flow control on a PASS filter rejected");
    CHECK_EQ(PassThruStartMsgFilter(ch, BLOCK_FILTER, &mask, &pat, &fc, &fid),
             ERR_INVALID_MSG, "flow control on a BLOCK filter rejected");
    CHECK_EQ(PassThruStartMsgFilter(ch, PASS_FILTER, &mask, &pat, NULL, &fid),
             STATUS_NOERROR, "PASS filter without flow control accepted");

    CHECK_EQ(PassThruStopMsgFilter(ch, 0), STATUS_NOERROR, "StopMsgFilter");
    shut(dev);
}

/* Defect 6: IOCTLs 4, 9, 10 and 11-14 were missing entirely. */
static void ioctls(void)
{
    J_U32 dev = open_device(), ch = 0, v = 0;
    SCONFIG cfg[2];
    SCONFIG_LIST list;

    CHECK_EQ(PassThruIoctl(dev, READ_VBATT, NULL, &v), STATUS_NOERROR,
             "READ_VBATT");
    CHECK_EQ(v, 12480, "READ_VBATT millivolts");

    CHECK_EQ(PassThruIoctl(dev, READ_PROG_VOLTAGE, NULL, &v), STATUS_NOERROR,
             "READ_PROG_VOLTAGE");

    PassThruConnect(dev, ISO15765, 0, 500000, &ch);

    cfg[0].Parameter = ISO15765_BS;    cfg[0].Value = 0;
    cfg[1].Parameter = ISO15765_STMIN; cfg[1].Value = 0;
    list.NumOfParams = 2;
    list.ConfigPtr   = cfg;
    CHECK_EQ(PassThruIoctl(ch, SET_CONFIG, &list, NULL), STATUS_NOERROR,
             "SET_CONFIG");
    CHECK(tx_contains("ats6 30 0 "), "SET_CONFIG encoding");

    cfg[0].Parameter = DATA_RATE;
    list.NumOfParams = 1;
    CHECK_EQ(PassThruIoctl(ch, GET_CONFIG, &list, NULL), STATUS_NOERROR,
             "GET_CONFIG");
    CHECK_EQ(cfg[0].Value, 500000, "GET_CONFIG returns the value");

    CHECK_EQ(PassThruIoctl(ch, SET_CONFIG, NULL, NULL), ERR_NULL_PARAMETER,
             "SET_CONFIG with a NULL list");

    CHECK_EQ(PassThruIoctl(ch, CLEAR_RX_BUFFER, NULL, NULL), STATUS_NOERROR,
             "CLEAR_RX_BUFFER");
    CHECK_EQ(PassThruIoctl(ch, CLEAR_TX_BUFFER, NULL, NULL), STATUS_NOERROR,
             "CLEAR_TX_BUFFER");
    CHECK_EQ(PassThruIoctl(ch, CLEAR_PERIODIC_MSGS, NULL, NULL), STATUS_NOERROR,
             "CLEAR_PERIODIC_MSGS is implemented");
    CHECK_EQ(PassThruIoctl(ch, CLEAR_MSG_FILTERS, NULL, NULL), STATUS_NOERROR,
             "CLEAR_MSG_FILTERS is implemented");

    /* J1850 only, and this device rejects J1850 outright. Saying so is better
     * than a silent success the caller cannot detect. */
    CHECK_EQ(PassThruIoctl(ch, CLEAR_FUNCT_MSG_LOOKUP_TABLE, NULL, NULL),
             ERR_NOT_SUPPORTED, "functional message table is refused");

    CHECK_EQ(PassThruIoctl(ch, 0xDEAD, NULL, NULL), ERR_INVALID_IOCTL_ID,
             "unknown ioctl id");
    shut(dev);
}

static void read_version(void)
{
    J_U32 dev = open_device();
    char fw[80], dll[80], api[80];

    CHECK_EQ(PassThruReadVersion(dev, fw, dll, api), STATUS_NOERROR,
             "ReadVersion");
    CHECK_STR(fw, "1.17.4877", "firmware version parsed from ari");
    CHECK(strlen(dll) > 0, "dll version present");
    CHECK_STR(api, "04.04", "api version");
    CHECK_EQ(PassThruReadVersion(dev, NULL, dll, api), ERR_NULL_PARAMETER,
             "ReadVersion(NULL)");
    shut(dev);
}

/*
 * Every read below asks for more messages than the wire will deliver, so the
 * conformant return is ERR_TIMEOUT with the messages that did arrive still
 * counted and delivered — not STATUS_NOERROR. Reporting success for a short
 * read would leave a caller unable to tell a finished exchange from an ECU
 * that went quiet.
 */
static void receive_path(void)
{
    J_U32 dev = open_device(), ch = 0, count;
    PASSTHRU_MSG got[4];

    PassThruConnect(dev, ISO15765, 0, 500000, &ch);

    /* Framing here follows what the vehicle actually sent (PROTOCOL.md §7,
     * measured 2026-06-17). tests/unit/test_golden.c replays the recorded
     * exchange byte for byte; this test covers each frame shape in isolation. */

    /* A single-frame message is one END frame. No START precedes it. */
    {
        const uint8_t f[] = { 'a','r','6', 0x0B, 0x40, 0,0,0x10,0x00,
                              0x00,0x00,0x07,0xE8, 0x7E, 0x00 };
        mock_push(f, sizeof f);
    }
    count = 4;
    CHECK_EQ(PassThruReadMsgs(ch, got, &count, 500), ERR_TIMEOUT,
             "single-frame message received");
    CHECK_EQ(count, 1, "one message");
    CHECK_EQ(got[0].DataSize, 6, "payload is CAN id plus data");
    CHECK_EQ(got[0].Data[3], 0xE8, "CAN id preserved");
    CHECK_EQ(got[0].Data[4], 0x7E, "payload preserved");
    CHECK_EQ(got[0].Timestamp, 0x1000, "timestamp preserved");
    CHECK_EQ(got[0].RxStatus, 0, "a complete message carries no indication bits");

    /* A segmented message: a START frame with the CAN id only announces it,
     * then END-terminated frames carry the id again plus the data. The
     * announcement is its own J2534 indication; the data is one message. */
    {
        const uint8_t a[] = { 'a','r','6', 0x09, 0x80, 0,0,0x20,0x00, 0x00,0x00,0x07,0xE8 };
        const uint8_t b[] = { 'a','r','6', 0x0B, 0x00, 0,0,0x20,0x01, 0x00,0x00,0x07,0xE8, 0xAA,0xBB };
        const uint8_t c[] = { 'a','r','6', 0x07, 0x40, 0,0,0x20,0x02, 0xCC,0xDD };
        mock_push(a, sizeof a);
        mock_push(b, sizeof b);
        mock_push(c, sizeof c);
    }
    count = 4;
    CHECK_EQ(PassThruReadMsgs(ch, got, &count, 500), ERR_TIMEOUT,
             "segmented message received");
    CHECK_EQ(count, 2, "an indication, then one reassembled message");
    CHECK_EQ(got[0].RxStatus, ISO15765_FIRST_FRAME, "first-frame indication flagged");
    CHECK_EQ(got[0].DataSize, 4, "indication carries the CAN id only");
    CHECK_EQ(got[1].RxStatus, 0, "data message carries no indication bits");
    CHECK_EQ(got[1].DataSize, 8, "reassembled length: id once, then data");
    CHECK_EQ(got[1].Data[3], 0xE8, "CAN id once at the front");
    CHECK_EQ(got[1].Data[4], 0xAA, "second frame appended");
    CHECK_EQ(got[1].Data[7], 0xDD, "third frame appended");
    CHECK_EQ(got[1].ExtraDataIndex, 8, "ExtraDataIndex = DataSize");

    /* A single START|END frame is still a complete message (unobserved on
     * receive; kept because it is what the bit definitions imply). */
    {
        const uint8_t f[] = { 'a','r','6', 0x0B, 0xC0, 0,0,0x30,0x00,
                              0x00,0x00,0x07,0xE8, 0x7E, 0x00 };
        mock_push(f, sizeof f);
    }
    count = 4;
    CHECK_EQ(PassThruReadMsgs(ch, got, &count, 500), ERR_TIMEOUT,
             "START|END frame received");
    CHECK_EQ(count, 1, "one message");
    CHECK_EQ(got[0].RxStatus, 0, "no indication bits on a complete message");
    CHECK_EQ(got[0].DataSize, 6, "payload intact");

    /* A transmit indication (0x10, id only) is delivered as TX_DONE and must
     * not be glued onto the message that follows it. */
    {
        const uint8_t ind[] = { 'a','r','6', 0x09, 0x10, 0,0,0x40,0x00, 0x00,0x00,0x07,0xE0 };
        const uint8_t rsp[] = { 'a','r','6', 0x0B, 0x40, 0,0,0x40,0x01,
                                0x00,0x00,0x07,0xE8, 0x7E, 0x00 };
        mock_push(ind, sizeof ind);
        mock_push(rsp, sizeof rsp);
    }
    count = 4;
    CHECK_EQ(PassThruReadMsgs(ch, got, &count, 500), ERR_TIMEOUT,
             "transmit indication then reply");
    CHECK_EQ(count, 2, "two messages");
    CHECK_EQ(got[0].RxStatus, TX_MSG_TYPE | TX_DONE, "TX_DONE indication");
    /* The device sends the CAN id with a transmit indication; J2534 Table 13
     * says a TxDone carries no data, and commands here are serialised so the
     * id adds no correlation the caller lacks. It stays in the log. */
    CHECK_EQ(got[0].DataSize, 0, "a TxDone indication carries no data");
    CHECK_EQ(got[0].ExtraDataIndex, 0, "and reports ExtraDataIndex zero");
    CHECK_EQ(got[1].RxStatus, 0, "the reply is a plain received message");
    CHECK_EQ(got[1].DataSize, 6, "the reply is not contaminated by the indication");
    CHECK_EQ(got[1].Data[3], 0xE8, "reply CAN id at the front, once");

    /* A transmit echo: START|LOOPBACK with the id announces it, then
     * END|LOOPBACK carries the echoed data. Both flagged as our own transmit. */
    {
        const uint8_t a[] = { 'a','r','6', 0x09, 0xA0, 0,0,0x50,0x00, 0x00,0x00,0x07,0xE0 };
        const uint8_t b[] = { 'a','r','6', 0x0B, 0x60, 0,0,0x50,0x01,
                              0x00,0x00,0x07,0xE0, 0x3E, 0x00 };
        mock_push(a, sizeof a);
        mock_push(b, sizeof b);
    }
    count = 4;
    CHECK_EQ(PassThruReadMsgs(ch, got, &count, 500), ERR_TIMEOUT,
             "loopback received");
    CHECK_EQ(count, 2, "echo indication then echo data");
    CHECK_EQ(got[0].RxStatus, TX_MSG_TYPE | TX_DONE, "echo start is TX_DONE");
    CHECK(got[1].RxStatus & TX_MSG_TYPE, "echo data sets TX_MSG_TYPE");
    CHECK(!(got[1].RxStatus & START_OF_MESSAGE), "echo data is not a first-frame marker");
    CHECK_EQ(got[1].DataSize, 6, "echo data intact");

    /* A START announcement while a reassembly is still open means the stream
     * lost an END frame. The stale partial must be dropped, not merged. */
    {
        const uint8_t dangling[] = { 'a','r','6', 0x0B, 0x00, 0,0,0x60,0x00,
                                     0x00,0x00,0x07,0xE8, 0x11, 0x22 };
        const uint8_t a[] = { 'a','r','6', 0x09, 0x80, 0,0,0x60,0x01, 0x00,0x00,0x07,0xE8 };
        const uint8_t b[] = { 'a','r','6', 0x0B, 0x40, 0,0,0x60,0x02,
                              0x00,0x00,0x07,0xE8, 0x7E, 0x00 };
        mock_push(dangling, sizeof dangling);
        mock_push(a, sizeof a);
        mock_push(b, sizeof b);
    }
    count = 4;
    CHECK_EQ(PassThruReadMsgs(ch, got, &count, 500), ERR_TIMEOUT,
             "recovery after a lost END frame");
    CHECK_EQ(count, 2, "indication and the new message only");
    CHECK_EQ(got[1].DataSize, 6, "the unterminated bytes were not glued on");
    CHECK_EQ(got[1].Data[4], 0x7E, "new message data intact");
    shut(dev);
}

static void filter_takes_flow_control_flags(void)
{
    /*
     * The device takes one TxFlags for a whole filter. For a flow-control
     * filter the flow-control message is the one it transmits, so an
     * application that sets ISO15765_FRAME_PAD only there — where the standard
     * puts it — must still get padded flow-control frames on the wire. Taking
     * the flags from the mask instead silently produced unpadded ones, which
     * a conforming ECU ignores (ISO 15765-4 clause 8.1), stalling every
     * segmented receive.
     */
    J_U32 dev = open_device(), ch = 0, fid = 0;
    PASSTHRU_MSG mask, pat, fc;

    PassThruConnect(dev, ISO15765, 0, 500000, &ch);
    memset(&mask, 0, sizeof mask); mask.DataSize = 4; mask.TxFlags = 0;
    memset(&pat,  0, sizeof pat);  pat.DataSize  = 4; pat.TxFlags  = 0;
    memset(&fc,   0, sizeof fc);   fc.DataSize   = 4;
    fc.TxFlags = ISO15765_FRAME_PAD;

    mock_clear_tx();
    CHECK_EQ(PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, &pat, &fc, &fid),
             STATUS_NOERROR, "flow-control filter installed");
    CHECK(tx_contains("atf6 3 64 4"),
          "the flow-control message's pad flag reaches the device");
    CHECK(!tx_contains("atf6 3 0 4"),
          "the mask's empty flags are not what got sent");

    /* A PASS filter transmits nothing, so the mask's flags stand. */
    mock_clear_tx();
    mask.TxFlags = 0;
    CHECK_EQ(PassThruStartMsgFilter(ch, PASS_FILTER, &mask, &pat, NULL, &fid),
             STATUS_NOERROR, "pass filter installed");
    CHECK(tx_contains("atf6 1 0 4"), "a pass filter still uses the mask's flags");
    shut(dev);
}

/* Acks every command with `aro` and never sends an `ary` init result: the
 * device accepted the command, the ECU said nothing. */
static void bare_ok_responder(const char *line, size_t len,
                              const uint8_t *payload, size_t payload_len)
{
    char buf[64];
    size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
    (void)payload; (void)payload_len;
    memcpy(buf, line, copy); buf[copy] = '\0';
    if (strncmp(buf, "ati", 3) == 0) mock_push_str("ari main code version : 1.17.4877\r\n");
    else mock_push_reply("aro");
}

static void kline_init_ioctls(void)
{
    J_U32 dev = open_device(), ch = 0;
    unsigned char addr = 0x33, keys[8];
    SBYTE_ARRAY in, out;
    PASSTHRU_MSG req, rsp;

    CHECK_EQ(PassThruConnect(dev, ISO9141, 0, 10400, &ch), STATUS_NOERROR,
             "Connect ISO9141");

    /* Five-baud: the address goes out in decimal on the command line, with
     * no payload; the two keybytes come back as `arw3 85 8 <seq>` and land
     * in the caller's SBYTE_ARRAY with the number stripped. */
    mock_clear_tx();
    in.NumOfBytes = 1; in.BytePtr = &addr;
    out.NumOfBytes = sizeof keys; out.BytePtr = keys;
    CHECK_EQ(PassThruIoctl(ch, FIVE_BAUD_INIT, &in, &out), STATUS_NOERROR, "FIVE_BAUD_INIT");
    CHECK(tx_contains("atw3 51 "), "five-baud command carries the address 0x33 as 51");
    CHECK_EQ(out.NumOfBytes, 2, "two keybytes returned");
    CHECK(keys[0] == 0x55 && keys[1] == 0x08, "keybytes are the device's reply bytes");

    /* Fast init: the request goes out as-is after "aty3 <len> 0"; the ECU's
     * StartCommunication response comes back as a message. */
    mock_clear_tx();
    memset(&req, 0, sizeof req);
    req.ProtocolID = ISO9141; req.DataSize = 4;
    req.Data[0] = 0xC1; req.Data[1] = 0x33; req.Data[2] = 0xF1; req.Data[3] = 0x81;
    CHECK_EQ(PassThruIoctl(ch, FAST_INIT, &req, &rsp), STATUS_NOERROR, "FAST_INIT");
    CHECK(tx_contains("aty3 4 0 "), "fast init command and request on the wire");
    CHECK_EQ(rsp.DataSize, 3, "response length");
    CHECK(rsp.Data[0] == 0xC1 && rsp.Data[2] == 0x8F, "response bytes");
    CHECK_EQ(rsp.ProtocolID, ISO9141, "response carries the channel's protocol");

    CHECK_EQ(PassThruIoctl(ch, FIVE_BAUD_INIT, NULL, &out), ERR_NULL_PARAMETER,
             "FIVE_BAUD_INIT(NULL input)");

    /* A fast init with nothing to send is the wake-up pattern alone, which the
     * standard permits and the firmware supports. */
    req.DataSize = 0;
    CHECK_EQ(PassThruIoctl(ch, FAST_INIT, &req, &rsp), STATUS_NOERROR,
             "FAST_INIT with an empty request is accepted");
    CHECK_EQ(PassThruIoctl(ch, FAST_INIT, NULL, &rsp), STATUS_NOERROR,
             "FAST_INIT with a NULL input is accepted");
    shut(dev);
}

/* A device that acknowledges the init command but reports no answer from the
 * ECU has not initialised anything. Saying otherwise would be the one lie this
 * driver is built to avoid. */
static void kline_init_failure(void)
{
    J_U32 dev, ch = 0;
    PASSTHRU_MSG req, rsp;
    SBYTE_ARRAY in, out;
    unsigned char addr = 0x33, keys[8];

    mock_reset();
    mock_set_responder(bare_ok_responder);
    op_device_set_factory(mock_open);
    CHECK_EQ(PassThruOpen(NULL, &dev), STATUS_NOERROR, "PassThruOpen");
    CHECK_EQ(PassThruConnect(dev, ISO9141, 0, 10400, &ch), STATUS_NOERROR, "Connect");

    memset(&rsp, 0xEE, sizeof rsp);
    memset(&req, 0, sizeof req);
    req.ProtocolID = ISO9141; req.DataSize = 4;
    req.Data[0] = 0xC1; req.Data[1] = 0x33; req.Data[2] = 0xF1; req.Data[3] = 0x81;
    CHECK_EQ(PassThruIoctl(ch, FAST_INIT, &req, &rsp), ERR_INIT_FAILED,
             "a fast init the ECU never answered reports ERR_INIT_FAILED");
    CHECK_EQ(rsp.Data[0], 0xEE, "the caller's output is untouched on failure");

    in.NumOfBytes = 1; in.BytePtr = &addr;
    out.NumOfBytes = sizeof keys; out.BytePtr = keys;
    CHECK_EQ(PassThruIoctl(ch, FIVE_BAUD_INIT, &in, &out), ERR_INIT_FAILED,
             "an unanswered five-baud init reports ERR_INIT_FAILED");
    CHECK_EQ(out.NumOfBytes, sizeof keys, "the caller's array is untouched on failure");
    shut(dev);
}

/*
 * A command that times out must not hand its late reply to the next command.
 *
 * Found by the differential harness against real hardware: a transmit the
 * device took 1.2 s to reject was picked up by the following StopMsgFilter,
 * which then reported the transmit's failure as its own. Every result after
 * that point belongs to the previous request — silently.
 */
static void late_reply_is_not_reused(void)
{
    J_U32 dev = open_device(), ch = 0, n = 1;
    PASSTHRU_MSG m;

    PassThruConnect(dev, ISO15765, 0, 500000, &ch);
    memset(&m, 0, sizeof m);
    m.DataSize = 6;

    /* A silent device: this write must time out. */
    mock_set_responder(NULL);
    n = 1;
    CHECK_EQ(PassThruWriteMsgs(ch, &m, &n, 100), ERR_TIMEOUT,
             "write times out against a silent device");

    /* The device answers late, after the caller has already given up. */
    mock_push_reply("are 9");
    usleep(120000);

    /* The next command must get its own reply, not the abandoned one. */
    mock_install_openport_responder();
    CHECK_EQ(PassThruStopMsgFilter(ch, 0), STATUS_NOERROR,
             "next command is unaffected by the abandoned reply");

    /* And the one after that, to prove the pipe stayed in step. */
    {
        J_U32 v = 0;
        CHECK_EQ(PassThruIoctl(dev, READ_VBATT, NULL, &v), STATUS_NOERROR,
                 "pipe still in step");
        CHECK_EQ(v, 12480, "value belongs to this command, not an earlier one");
    }
    shut(dev);
}

/*
 * Timeout bounds the CALL, not each message.
 *
 * Handing every message the full value would let a five-message write take
 * five times as long as the caller allowed. On this device a transmit that
 * gets no bus acknowledgement takes over a second to fail, so the overrun is
 * real: a caller budgeting 200 ms could block for six seconds mid-reflash.
 */
static void write_timeout_is_a_call_budget(void)
{
    J_U32 dev = open_device(), ch = 0, n;
    PASSTHRU_MSG msgs[5];
    struct timeval t0, t1;
    long elapsed;
    int i;

    PassThruConnect(dev, ISO15765, 0, 500000, &ch);
    memset(msgs, 0, sizeof msgs);
    for (i = 0; i < 5; i++) msgs[i].DataSize = 6;

    /* Writes SUCCEED, but take 120 ms each. A silent device would not exercise
     * this: WriteMsgs returns on the first failure and never reaches message
     * two, so only slow-but-working writes distinguish a per-call budget from
     * a per-message one. */
    mock_delay_transmits(120);
    n = 5;
    gettimeofday(&t0, NULL);
    {
        long rc = PassThruWriteMsgs(ch, msgs, &n, 300);
        gettimeofday(&t1, NULL);
        elapsed = (long)((t1.tv_sec - t0.tv_sec) * 1000L +
                         (t1.tv_usec - t0.tv_usec) / 1000L);
        CHECK(rc == ERR_TIMEOUT,
              "5 x 120 ms of writes against a 300 ms budget reports ERR_TIMEOUT "
              "(got %ld)", rc);
    }
    CHECK(elapsed < 500,
          "the call honoured its 300 ms budget (took %ld ms; per-message would "
          "be ~600)", elapsed);
    CHECK(n > 0 && n < 5,
          "and reported the partial count (%lu of 5)", (unsigned long)n);

    mock_delay_transmits(0);
    shut(dev);
}

/*
 * A queue overrun must be reported, not swallowed.
 *
 * Dropping messages and returning success is a silent loss of vehicle data:
 * the caller's view of the bus has a hole in it and no way to know.
 */
static void queue_overrun_is_reported(void)
{
    J_U32 dev = open_device(), ch = 0, count;
    PASSTHRU_MSG got[8];
    char err[80];
    int i;
    int saw_overflow = 0;

    PassThruConnect(dev, ISO15765, 0, 500000, &ch);

    /* More messages than the queue holds, delivered while nobody is reading. */
    for (i = 0; i < OP_RXQ_DEPTH + 16; i++) {
        uint8_t f[] = { 'a','r','6', 0x0B, 0xC0, 0,0,0x10,(uint8_t)i,
                        0x00,0x00,0x07,0xE8, 0x7E, 0x00 };
        mock_push(f, sizeof f);
    }
    usleep(300000);                        /* let the reader drain the pipe */

    for (i = 0; i < 4 && !saw_overflow; i++) {
        count = 8;
        if (PassThruReadMsgs(ch, got, &count, 100) == ERR_BUFFER_OVERFLOW)
            saw_overflow = 1;
    }
    CHECK(saw_overflow, "ReadMsgs reports ERR_BUFFER_OVERFLOW after an overrun");
    if (saw_overflow) {
        memset(err, 0, sizeof err);
        PassThruGetLastError(err);
        CHECK(strstr(err, "dropped") != NULL,
              "and says how many were lost: \"%s\"", err);
    }
    shut(dev);
}

/* ---- sequence numbers --------------------------------------------------- */

/* Answers every numbered command twice: first with a stale reply carrying the
 * previous number (as a backlog from an earlier session would), then with the
 * real one. A transmit is answered with a failure, so a driver that took the
 * wrong reply would report it on the next call instead. */
static void stale_then_real_responder(const char *line, size_t len,
                                      const uint8_t *payload, size_t payload_len)
{
    char buf[64], stale[64];
    size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
    const char *sp; unsigned long seq = 0;
    (void)payload; (void)payload_len;
    memcpy(buf, line, copy); buf[copy] = '\0';
    if (strncmp(buf, "ati", 3) == 0) { mock_push_str("ari main code version : 1.17.4877\r\n"); return; }
    sp = strrchr(buf, ' ');
    if (sp != NULL) seq = strtoul(sp + 1, NULL, 10);
    if (strncmp(buf, "atr", 3) == 0) {
        snprintf(stale, sizeof stale, "arr 16 999 %lu\r\n", seq - 1);
        mock_push_str(stale);
        mock_push_str("arr 16 12480\r\n");             /* unnumbered: also stale */
        mock_push_reply("arr 16 12480");
    } else if (strncmp(buf, "att", 3) == 0) {
        mock_push_reply("are 9");
    } else {
        snprintf(stale, sizeof stale, "aro %lu\r\n", seq - 1);
        mock_push_str(stale);
        mock_push_reply("aro");
    }
}

static void sequence_numbers_are_matched(void)
{
    J_U32 dev = 0, ch = 0, v = 0, n = 1;
    PASSTHRU_MSG m;

    mock_reset();
    mock_set_responder(stale_then_real_responder);
    op_device_set_factory(mock_open);
    CHECK_EQ(PassThruOpen(NULL, &dev), STATUS_NOERROR, "Open despite a stale reply before every real one");
    CHECK_EQ(PassThruIoctl(dev, READ_VBATT, NULL, &v), STATUS_NOERROR, "READ_VBATT");
    CHECK_EQ(v, 12480, "the numbered reply is taken, not the stale or the unnumbered one");
    CHECK_EQ(PassThruConnect(dev, ISO15765, 0, 500000, &ch), STATUS_NOERROR, "Connect");

    /* Fire-and-forget: the transmit's failure reply must not become the
     * answer to the disconnect that follows it. */
    memset(&m, 0, sizeof m);
    m.ProtocolID = ISO15765; m.DataSize = 6; m.Data[2] = 0x07; m.Data[3] = 0xE0; m.Data[4] = 0x3E;
    CHECK_EQ(PassThruWriteMsgs(ch, &m, &n, 0), STATUS_NOERROR, "WriteMsgs with Timeout 0 returns at once");
    CHECK_EQ(n, 1, "and reports the message as queued");
    CHECK(tx_contains("att6 6 0 1000000 "), "the transmit went out numbered, with the 1 s budget a zero timeout maps to");
    CHECK_EQ(PassThruDisconnect(ch), STATUS_NOERROR,
             "Disconnect gets its own reply, not the transmit's late failure");
    CHECK_EQ(PassThruClose(dev), STATUS_NOERROR, "Close");
    CHECK_EQ(PassThruClose(dev), ERR_INVALID_DEVICE_ID, "Close again: invalid device id");
    op_device_set_factory(NULL);
}

/* ---- call recording ------------------------------------------------------ */
static void calls_are_recorded(void)
{
    char path[] = "/tmp/op_rec_XXXXXX";
    int fd = mkstemp(path);
    J_U32 dev, ch = 0, n = 1; PASSTHRU_MSG m; FILE *f; char line[512]; int lines = 0;
    int saw_open = 0, saw_result = 0, saw_write = 0, saw_ioctl = 0;
    if (fd < 0) { CHECK(0, "mkstemp"); return; }
    close(fd);
    setenv("OPENPORT_RECORD", path, 1);
    dev = open_device();
    PassThruConnect(dev, ISO15765, 0, 500000, &ch);
    {
        SCONFIG c[2] = { { ISO15765_BS, 0 }, { ISO15765_STMIN, 5 } };
        SCONFIG_LIST l = { 2, c };
        PassThruIoctl(ch, SET_CONFIG, &l, NULL);
    }
    memset(&m, 0, sizeof m);
    m.ProtocolID = ISO15765; m.TxFlags = ISO15765_FRAME_PAD; m.DataSize = 6;
    m.Data[2] = 0x07; m.Data[3] = 0xE0; m.Data[4] = 0x3E; m.Data[5] = 0x01;
    PassThruWriteMsgs(ch, &m, &n, 1000);
    shut(dev);
    unsetenv("OPENPORT_RECORD");
    f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        lines++;
        if (!strcmp(line, "open\n")) saw_open = 1;
        if (!strcmp(line, "= 6\n")) saw_result = 1;
        if (!strcmp(line, "write 6 1 1000 6 64 000007e03e01\n")) saw_write = 1;
        if (!strcmp(line, "ioctl 6 2 30=0 31=5\n")) saw_ioctl = 1;
    }
    if (f) fclose(f);
    unlink(path);
    CHECK(lines >= 6, "recorder wrote one line per call");
    CHECK(saw_open, "open is recorded");
    CHECK(saw_result, "the channel id handed out is recorded as '= 6'");
    CHECK(saw_ioctl, "SET_CONFIG parameters are recorded");
    CHECK(saw_write, "a write is recorded with protocol, flags and payload");
}

void test_j2534(void)
{
    SUITE("J2534 entry points");
    sequence_numbers_are_matched();
    calls_are_recorded();
    null_parameters();
    connect_and_channels();
    device_absent();
    filters();
    ioctls();
    read_version();
    last_error_is_useful();
    receive_path();
    filter_takes_flow_control_flags();
    kline_init_ioctls();
    kline_init_failure();
    timeouts_are_honoured();
    transport_failures_propagate();
    periodic_is_real();
    periodic_stop_waits_for_inflight();
    programming_voltage();
    late_reply_is_not_reused();
    write_timeout_is_a_call_budget();
    queue_overrun_is_reported();
}
