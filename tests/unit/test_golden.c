/*
 * Golden traces: frame sequences a vehicle actually sent, replayed through the
 * shipped receive path, judged by the acceptance rule of a real consumer.
 *
 * The receive framing was first modelled from the bit definitions and the
 * model was wrong in a way no bench, unit test or simulator could see, because
 * all three were built from the same model. The only defence against that is a
 * fixture that comes from a measurement rather than from the code under test.
 * Every sequence here is transcribed from a recorded session; the source is
 * cited beside it. Do not "fix" a fixture to make a test pass.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"
#include "j2534/j2534.h"
#include "op_device.h"
#include "mock_transport.h"

#include <stdlib.h>
#include <string.h>

/*
 * The acceptance rule a real consuming application applies to what
 * PassThruReadMsgs returns, reproduced exactly. A message survives it only if
 * it is neither an indication nor an echo, carries at least a CAN id, and that
 * id is the one the flow-control filter was armed for. Anything else is dropped.
 */
static const unsigned char *consumer_accept(const PASSTHRU_MSG *m, J_U32 rx_id,
                                            size_t *len)
{
    J_U32 id;
    if (m->RxStatus & (TX_MSG_TYPE | START_OF_MESSAGE | ISO15765_PADDING_ERROR))
        return NULL;
    if (m->DataSize < 4) return NULL;
    id = ((J_U32)m->Data[0] << 24) | ((J_U32)m->Data[1] << 16) |
         ((J_U32)m->Data[2] << 8)  |  (J_U32)m->Data[3];
    if (id != rx_id) return NULL;
    *len = m->DataSize - 4;
    return m->Data + 4;
}

/* Build one wire frame: 'a' 'r' <ch> <len> <status> <ts be32> <payload>. */
static void push_frame_ch(char chan, uint8_t status, uint32_t ts,
                          const uint8_t *payload, size_t n)
{
    uint8_t f[4 + 255];
    f[0] = 'a'; f[1] = 'r'; f[2] = (uint8_t)chan;
    f[3] = (uint8_t)(1 + 4 + n);
    f[4] = status;
    f[5] = (uint8_t)(ts >> 24); f[6] = (uint8_t)(ts >> 16);
    f[7] = (uint8_t)(ts >> 8);  f[8] = (uint8_t)ts;
    memcpy(f + 9, payload, n);
    mock_push(f, 9 + n);
}

static void push_frame(uint8_t status, uint32_t ts, const uint8_t *payload, size_t n)
{
    push_frame_ch('6', status, ts, payload, n);
}

/*
 * A live ISO15765 multi-frame reply, transcribed from a 2012 VW Caddy over the
 * shipped driver on 2026-09-13. This is the exchange the whole
 * receive-framing correction turned on, captured on real hardware for the first
 * time rather than borrowed from the prior driver: a ReadDataByIdentifier F190
 * (VIN) reply arrives as a START frame carrying the CAN id alone, then an
 * END frame carrying the id again followed by the data. The driver must deliver
 * the announcement as its own ISO15765_FIRST_FRAME indication and the data as
 * one clean message with the id present once. The prior driver merged them and
 * a consumer discarded the result.
 */
static const uint8_t k_caddy_rx_id[4] = { 0x00, 0x00, 0x07, 0xE8 };
#define CADDY_RX_ID 0x7E8u

static void caddy_vin_replay(void)
{
    /* 62 F1 90 then a 17-character VIN; the vehicle's own is replaced by a
     * documentation VIN of the same length. */
    static const uint8_t vin_resp[] = {
        0x62, 0xF1, 0x90, 'W','V','W','Z','Z','Z','1','J','Z','3','W',
        '3','8','6','7','5','2' };
    J_U32 dev = 0, ch = 0, count = 8, k, accepted = 0;
    PASSTHRU_MSG got[8];

    mock_reset();
    mock_install_openport_responder();
    op_device_set_factory(mock_open);
    CHECK_EQ(PassThruOpen(NULL, &dev), STATUS_NOERROR, "PassThruOpen");
    CHECK_EQ(PassThruConnect(dev, ISO15765, 0, 500000, &ch), STATUS_NOERROR,
             "Connect ISO15765");

    /* The wire exactly as the Caddy sent it: id-only START, then id+data END. */
    push_frame(0x80, 0x1000, k_caddy_rx_id, 4);
    {
        uint8_t end[4 + sizeof vin_resp];
        memcpy(end, k_caddy_rx_id, 4);
        memcpy(end + 4, vin_resp, sizeof vin_resp);
        push_frame(0x40, 0x1000, end, sizeof end);
    }

    CHECK_EQ(PassThruReadMsgs(ch, got, &count, 500), ERR_TIMEOUT, "ReadMsgs");
    CHECK_EQ(count, 2, "an indication, then one reassembled message");
    CHECK_EQ(got[0].RxStatus, ISO15765_FIRST_FRAME, "first-frame indication flagged");
    CHECK_EQ(got[0].DataSize, 4, "indication carries the CAN id only");

    for (k = 0; k < count; k++) {
        size_t n = 0;
        const unsigned char *body = consumer_accept(&got[k], CADDY_RX_ID, &n);
        if (body == NULL) continue;
        accepted++;
        CHECK_EQ(n, sizeof vin_resp, "consumer sees the full reassembled reply");
        CHECK(n == sizeof vin_resp && memcmp(body, vin_resp, n) == 0,
              "the VIN reply is intact, id present once");
    }
    CHECK_EQ(accepted, 1, "exactly one message survives the consumer's rule");

    PassThruClose(dev);
    op_device_set_factory(NULL);
}

/*
 * Raw CAN, transcribed from the same 2012 VW Caddy. Raw CAN carries no
 * transport layer, so
 * the firmware marks each frame neither START nor END — the status byte is
 * 0x00 — and one wire frame is one whole message. The driver used to wait for
 * an END bit that never arrives and swallowed every frame, so
 * PassThruReadMsgs returned ERR_BUFFER_EMPTY on a bus that was answering.
 */
static void caddy_raw_can_replay(void)
{
    /* Our own request echoed back, then the ECU's reply. Both status 0x00. */
    static const uint8_t echo[]  = { 0x00,0x00,0x07,0xDF, 0x02,0x01,0x00,0x55,0x55,0x55,0x55,0x55 };
    static const uint8_t reply[] = { 0x00,0x00,0x07,0xE8, 0x06,0x41,0x00,0x98,0x3B,0xA0,0x13,0x00 };
    J_U32 dev = 0, ch = 0, count = 8;
    PASSTHRU_MSG got[8];

    mock_reset();
    mock_install_openport_responder();
    op_device_set_factory(mock_open);
    CHECK_EQ(PassThruOpen(NULL, &dev), STATUS_NOERROR, "PassThruOpen");
    CHECK_EQ(PassThruConnect(dev, CAN, 0, 500000, &ch), STATUS_NOERROR, "Connect raw CAN");

    push_frame_ch('5', 0x00, 0x2000, echo, sizeof echo);
    push_frame_ch('5', 0x00, 0x2001, reply, sizeof reply);

    CHECK_EQ(PassThruReadMsgs(ch, got, &count, 500), ERR_TIMEOUT,
             "raw CAN frames are delivered, not swallowed");
    CHECK_EQ(count, 2, "one message per wire frame");
    CHECK_EQ(got[0].DataSize, sizeof echo, "first frame complete");
    CHECK_EQ(got[1].DataSize, sizeof reply, "second frame complete");
    CHECK_EQ(got[1].RxStatus, 0, "a received raw CAN frame carries no indication bits");
    CHECK_EQ(got[1].ExtraDataIndex, got[1].DataSize, "ExtraDataIndex equals DataSize");
    CHECK(memcmp(got[1].Data, reply, sizeof reply) == 0, "reply bytes intact, id first");

    PassThruClose(dev);
    op_device_set_factory(NULL);
}

/*
 * The status byte's 0x02 bit, measured on a 2012 VW Caddy 2026-09-13.
 * Transmitting with TxFlags 0x00 and
 * 0x40 produced status 0x10; adding CAN_29BIT_ID (0x100) produced 0x12, with
 * and without padding. The bit tracks the 29-bit identifier exactly. Two
 * third-party drivers document it as J2534's START_OF_MESSAGE, which this
 * measurement refutes — 0x12 would then mean "transmit done and also the start
 * of a message", which is not a thing.
 */
static void caddy_29bit_marker(void)
{
    static const uint8_t id[4] = { 0x00, 0x00, 0x07, 0xE0 };
    J_U32 dev = 0, ch = 0, count = 4;
    PASSTHRU_MSG got[4];

    mock_reset();
    mock_install_openport_responder();
    op_device_set_factory(mock_open);
    PassThruOpen(NULL, &dev);
    PassThruConnect(dev, ISO15765, 0, 500000, &ch);

    push_frame(0x10, 0x3000, id, 4);       /* 11-bit transmit indication */
    push_frame(0x12, 0x3001, id, 4);       /* the same, 29-bit */
    PassThruReadMsgs(ch, got, &count, 500);
    CHECK_EQ(count, 2, "both indications delivered");
    CHECK_EQ(got[0].RxStatus & CAN_29BIT_ID, 0, "0x10 is an 11-bit identifier");
    CHECK(got[1].RxStatus & CAN_29BIT_ID, "0x12 reports CAN_29BIT_ID");
    CHECK(!(got[1].RxStatus & ISO15765_FIRST_FRAME),
          "and 0x02 is not read as a first-frame indication");

    PassThruClose(dev);
    op_device_set_factory(NULL);
}

void test_golden(void)
{
    SUITE("golden traces from vehicles");
    caddy_vin_replay();
    caddy_raw_can_replay();
    caddy_29bit_marker();
}
