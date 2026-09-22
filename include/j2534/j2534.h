/*
 * j2534.h — SAE J2534-1 PassThru API.
 *
 * Written from the SAE J2534-1 interface definition. Constants, structure
 * layouts and function signatures are the standard's, which is what makes an
 * application built against one J2534 library work against another; nothing
 * here is derived from any vendor implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef J2534_H
#define J2534_H

#ifdef __cplusplus
extern "C" {
#endif

/* J2534 fixes these as 32-bit on the wire between application and library.
 * "unsigned long" is 64-bit on LP64 (macOS/Linux arm64 + x86_64); the whole
 * ecosystem — including every consumer we care about — declares these APIs
 * with unsigned long, so we match that and stay ABI-compatible on this
 * platform rather than silently disagreeing with our callers. */
typedef unsigned long J_U32;

/* ---- ProtocolID ------------------------------------------------------- */
#define J1850VPW                    1UL
#define J1850PWM                    2UL
#define ISO9141                     3UL
#define ISO14230                    4UL
#define CAN                         5UL
#define ISO15765                    6UL
#define SCI_A_ENGINE                7UL
#define SCI_A_TRANS                 8UL
#define SCI_B_ENGINE                9UL
#define SCI_B_TRANS                10UL

/* ---- Return values ---------------------------------------------------- */
#define STATUS_NOERROR              0x00UL
#define ERR_NOT_SUPPORTED           0x01UL
#define ERR_INVALID_CHANNEL_ID      0x02UL
#define ERR_INVALID_PROTOCOL_ID     0x03UL
#define ERR_NULL_PARAMETER          0x04UL
#define ERR_INVALID_IOCTL_VALUE     0x05UL
#define ERR_INVALID_FLAGS           0x06UL
#define ERR_FAILED                  0x07UL
#define ERR_DEVICE_NOT_CONNECTED    0x08UL
#define ERR_TIMEOUT                 0x09UL
#define ERR_INVALID_MSG             0x0AUL
#define ERR_INVALID_TIME_INTERVAL   0x0BUL
#define ERR_EXCEEDED_LIMIT          0x0CUL
#define ERR_INVALID_MSG_ID          0x0DUL
#define ERR_DEVICE_IN_USE           0x0EUL
#define ERR_INVALID_IOCTL_ID        0x0FUL
#define ERR_BUFFER_EMPTY            0x10UL
#define ERR_BUFFER_FULL             0x11UL
#define ERR_BUFFER_OVERFLOW         0x12UL
#define ERR_PIN_INVALID             0x13UL
#define ERR_CHANNEL_IN_USE          0x14UL
#define ERR_MSG_PROTOCOL_ID         0x15UL
#define ERR_INVALID_FILTER_ID       0x16UL
#define ERR_NO_FLOW_CONTROL         0x17UL
#define ERR_NOT_UNIQUE              0x18UL
#define ERR_INVALID_BAUDRATE        0x19UL
#define ERR_INVALID_DEVICE_ID       0x1AUL
/* Named by later revisions than the 04.04 API reported here, but the right
 * answer when a K-line initialisation does not complete: an interface must not
 * report success for a wake-up the ECU never answered. */
#define ERR_INIT_FAILED             0x21UL
/* Codes 0x1B-0x27 are defined by later revisions of the standard than the
 * 04.04 API this driver reports. The device emits raw J2534 numbers, so they
 * are passed to the caller rather than collapsed; this is the ceiling of the
 * range treated as a real return code. */
#define ERR_J2534_HIGHEST           0x27UL

/* ---- IOCTL IDs -------------------------------------------------------- */
#define GET_CONFIG                  0x01UL
#define SET_CONFIG                  0x02UL
#define READ_VBATT                  0x03UL
#define FIVE_BAUD_INIT              0x04UL
#define FAST_INIT                   0x05UL
#define CLEAR_TX_BUFFER             0x07UL
#define CLEAR_RX_BUFFER             0x08UL
#define CLEAR_PERIODIC_MSGS         0x09UL
#define CLEAR_MSG_FILTERS           0x0AUL
#define CLEAR_FUNCT_MSG_LOOKUP_TABLE 0x0BUL
#define ADD_TO_FUNCT_MSG_LOOKUP_TABLE 0x0CUL
#define DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE 0x0DUL
#define READ_PROG_VOLTAGE           0x0EUL

/* ---- Configuration parameters ----------------------------------------- */
#define DATA_RATE                   0x01UL
#define LOOPBACK                    0x03UL
#define NODE_ADDRESS                0x04UL
#define NETWORK_LINE                0x05UL
#define P1_MIN                      0x06UL
#define P1_MAX                      0x07UL
#define P2_MIN                      0x08UL
#define P2_MAX                      0x09UL
#define P3_MIN                      0x0AUL
#define P3_MAX                      0x0BUL
#define P4_MIN                      0x0CUL
#define P4_MAX                      0x0DUL
#define W1                          0x0EUL
#define W2                          0x0FUL
#define W3                          0x10UL
#define W4                          0x11UL
#define W5                          0x12UL
#define TIDLE                       0x13UL
#define TINIL                       0x14UL
#define TWUP                        0x15UL
#define PARITY                      0x16UL
#define BIT_SAMPLE_POINT            0x17UL
#define SYNC_JUMP_WIDTH             0x18UL
#define W0                          0x19UL
#define T1_MAX                      0x1AUL
#define T2_MAX                      0x1BUL
#define T4_MAX                      0x1CUL
#define T5_MAX                      0x1DUL
#define ISO15765_BS                 0x1EUL
#define ISO15765_STMIN              0x1FUL
#define DATA_BITS                   0x20UL
#define FIVE_BAUD_MOD               0x21UL
#define BS_TX                       0x22UL
#define STMIN_TX                    0x23UL
#define T3_MAX                      0x24UL
#define ISO15765_WFT_MAX            0x25UL
/* The byte used to pad when ISO15765_FRAME_PAD is set; J2534 defaults it to
 * 0x00. Padding matters because ISO 15765-4 clause 8.1 requires every
 * diagnostic CAN frame to carry a DLC of eight, and says a receiver shall
 * ignore one that does not. Passed through to the device unchanged. */
#define ISO15765_PAD_VALUE          0x2BUL

/* ---- Filter types ----------------------------------------------------- */
#define PASS_FILTER                 0x01UL
#define BLOCK_FILTER                0x02UL
#define FLOW_CONTROL_FILTER         0x03UL

/* ---- Connect flags ---------------------------------------------------- */
#define CAN_29BIT_ID                0x00000100UL
#define ISO9141_NO_CHECKSUM         0x00000200UL
#define CAN_ID_BOTH                 0x00000800UL
#define ISO9141_K_LINE_ONLY         0x00001000UL

/* ---- RxStatus bits ---------------------------------------------------- */
#define TX_MSG_TYPE                 0x00000001UL
#define START_OF_MESSAGE            0x00000002UL
#define ISO15765_FIRST_FRAME        0x00000002UL
#define RX_BREAK                    0x00000004UL
#define TX_DONE                     0x00000008UL
#define ISO15765_PADDING_ERROR      0x00000010UL
#define ISO15765_ADDR_TYPE          0x00000080UL

/* ---- TxFlags bits ----------------------------------------------------- */
#define ISO15765_FRAME_PAD          0x00000040UL
#define ISO15765_ADDR_TYPE_TX       0x00000080UL
#define CAN_29BIT_ID_TX             0x00000100UL
#define WAIT_P3_MIN_ONLY            0x00000200UL
#define SW_CAN_HV_TX                0x00000400UL
#define SCI_MODE                    0x00400000UL
#define SCI_TX_VOLTAGE              0x00800000UL

/* Largest J2534 message payload. */
#define J2534_MSG_DATA_MAX          4128

/* ---- Values from Tactrix's own header (j2534_tactrix.h, shipped with the
 * official driver installer). Documented here so callers written against
 * the vendor DLL find the same names; only the ones marked implemented are
 * honoured by this driver. --------------------------------------------- */
#define SNIFF_MODE                  0x10000000UL /* Connect flag: listen without ACK. Passed through; firmware 1.17.4877 accepts it and still acknowledges */
/* J2534-2 channel ids. Tactrix's DLL opens them on these firmware channels. */
#define CAN_CH1                     0x00009000UL /* firmware 5, same as CAN */
#define ISO9141_CH1                 0x00009240UL /* firmware 3, K line, same as ISO9141 */
#define ISO9141_CH2                 0x00009241UL /* firmware 7, L line */
#define ISO9141_CH3                 0x00009242UL /* firmware 9, RS-232 receive on the 2.5 mm jack */
#define ISO9141_K                   ISO9141_CH1
#define ISO9141_L                   ISO9141_CH2
#define ISO9141_INNO                ISO9141_CH3
#define ISO14230_CH1                0x00009320UL /* firmware 4, K line, same as ISO14230 */
#define ISO14230_CH2                0x00009321UL /* firmware 8, L line */
#define ISO14230_K                  ISO14230_CH1
#define ISO14230_L                  ISO14230_CH2
#define ISO15765_CH1                0x00009400UL /* firmware 6, same as ISO15765 */
#define TX_PARAM_STOP_BITS          0x9000UL     /* GET/SET_CONFIG: serial stop bits, 1 by default (passed through) */
#define ISO15765_EXT_ADDR           0x00000080UL /* RxStatus alias of ISO15765_ADDR_TYPE */
#define VOLTAGE_OFF                 0xFFFFFFFFUL /* SetProgrammingVoltage: pin off (implemented) */
#define SHORT_TO_GROUND             0xFFFFFFFEUL /* SetProgrammingVoltage: pin to ground (refused on K under a K-line channel, L under an L-line one) */
#define PIN_VADJ                    17UL         /* adjustable output supply, not a J1962 pin; READ_PROG_VOLTAGE with pInput -> 17 */
#define CAN_MIXED_FORMAT            0x8000UL     /* SET_CONFIG: 0 off, 1 on, 2 all frames (passed through) */
#define ERR_OEM_VOLTAGE_TOO_HIGH    0x77UL       /* device error for SetProgrammingVoltage: above 20000 mV */
#define ERR_OEM_VOLTAGE_TOO_LOW     0x78UL       /* below 5000 mV */
#define TX_IOCTL_BASE               0x70000UL    /* Tactrix-private IOCTLs (not implemented) */
#define TX_IOCTL_APP_SERVICE        (TX_IOCTL_BASE + 0)
#define TX_IOCTL_SET_DLL_DEBUG_FLAGS (TX_IOCTL_BASE + 1)
#define TX_IOCTL_SET_DEV_DEBUG_FLAGS (TX_IOCTL_BASE + 2)
#define TX_IOCTL_SET_DLL_STATUS_CALLBACK (TX_IOCTL_BASE + 3)
#define TX_IOCTL_GET_DEVICE_INSTANCES (TX_IOCTL_BASE + 4)

typedef struct {
    J_U32 Parameter;
    J_U32 Value;
} SCONFIG;

typedef struct {
    J_U32    NumOfParams;
    SCONFIG *ConfigPtr;
} SCONFIG_LIST;

typedef struct {
    J_U32         NumOfBytes;
    unsigned char *BytePtr;
} SBYTE_ARRAY;

typedef struct {
    J_U32         ProtocolID;
    J_U32         RxStatus;
    J_U32         TxFlags;
    J_U32         Timestamp;
    J_U32         DataSize;
    J_U32         ExtraDataIndex;
    unsigned char Data[J2534_MSG_DATA_MAX];
} PASSTHRU_MSG;

/* ---- The 14 entry points ---------------------------------------------- */
long PassThruOpen(const void *pName, J_U32 *pDeviceID);
long PassThruClose(J_U32 DeviceID);
long PassThruConnect(J_U32 DeviceID, J_U32 ProtocolID, J_U32 Flags,
                     J_U32 BaudRate, J_U32 *pChannelID);
long PassThruDisconnect(J_U32 ChannelID);
long PassThruReadMsgs(J_U32 ChannelID, PASSTHRU_MSG *pMsg,
                      J_U32 *pNumMsgs, J_U32 Timeout);
long PassThruWriteMsgs(J_U32 ChannelID, const PASSTHRU_MSG *pMsg,
                       J_U32 *pNumMsgs, J_U32 Timeout);
long PassThruStartPeriodicMsg(J_U32 ChannelID, const PASSTHRU_MSG *pMsg,
                              J_U32 *pMsgID, J_U32 TimeInterval);
long PassThruStopPeriodicMsg(J_U32 ChannelID, J_U32 MsgID);
long PassThruStartMsgFilter(J_U32 ChannelID, J_U32 FilterType,
                            const PASSTHRU_MSG *pMaskMsg,
                            const PASSTHRU_MSG *pPatternMsg,
                            const PASSTHRU_MSG *pFlowControlMsg,
                            J_U32 *pFilterID);
long PassThruStopMsgFilter(J_U32 ChannelID, J_U32 FilterID);
long PassThruSetProgrammingVoltage(J_U32 DeviceID, J_U32 PinNumber, J_U32 Voltage);
long PassThruReadVersion(J_U32 DeviceID, char *pFirmwareVersion,
                         char *pDllVersion, char *pApiVersion);
long PassThruGetLastError(char *pErrorDescription);
long PassThruIoctl(J_U32 ChannelID, J_U32 IoctlID,
                   const void *pInput, void *pOutput);

#ifdef __cplusplus
}
#endif
#endif /* J2534_H */
