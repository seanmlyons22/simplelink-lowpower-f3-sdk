/*
 * Copyright (c) 2021-2026, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef ti_drivers_rcl_commands_generic__include
#define ti_drivers_rcl_commands_generic__include

#include <ti/drivers/rcl/RCL_Command.h>
#include <ti/drivers/rcl/RCL_Buffer.h>
#include <ti/drivers/rcl/handlers/generic.h>
#include <ti/drivers/rcl/LRF.h>

#include <ti/drivers/utils/List.h>


typedef struct RCL_CMD_GENERIC_FS_t            RCL_CmdGenericFs;
typedef struct RCL_CMD_GENERIC_FS_OFF_t        RCL_CmdGenericFsOff;
typedef struct RCL_CMD_GENERIC_TX_t            RCL_CmdGenericTx;
typedef struct RCL_CMD_GENERIC_TX_REPEAT_t     RCL_CmdGenericTxRepeat;
typedef struct RCL_CMD_GENERIC_TX_BURST_t      RCL_CmdGenericTxBurst;
typedef struct RCL_CMD_GENERIC_RX_BURST_t      RCL_CmdGenericRxBurst;
typedef struct RCL_CMD_GENERIC_TX_STREAM_t     RCL_CmdGenericTxStream;
typedef struct RCL_CMD_GENERIC_RX_STREAM_t     RCL_CmdGenericRxStream;
typedef struct RCL_CMD_GENERIC_TX_TEST_t       RCL_CmdGenericTxTest;
typedef struct RCL_CMD_GENERIC_RX_t            RCL_CmdGenericRx;
typedef struct RCL_CMD_GENERIC_LRF_OPERATION_t RCL_CmdGenericLrfOperation;
typedef struct RCL_STATS_GENERIC_t             RCL_StatsGeneric;
typedef struct RCL_STATS_GENERIC_TX_BURST_t    RCL_StatsGenericTxBurst;
typedef struct RCL_STATS_GENERIC_RX_BURST_t    RCL_StatsGenericRxBurst;
typedef struct RCL_STATS_GENERIC_TX_STREAM_t   RCL_StatsGenericTxStream;
typedef struct RCL_STATS_GENERIC_RX_STREAM_t   RCL_StatsGenericRxStream;

/**
 *  @brief Words in one row of a stream command's hop table
 *
 *  A row is everything LRF_programFrequency() writes for one frequency; the
 *  handler of a hopping stream captures one per channel at setup into the
 *  command's %hopRows and writes one at every hop. See RCL_CmdGenericTxStream.
 */
#define RCL_GENERIC_HOP_ROW_WORDS 19U
typedef struct RCL_CMD_NESB_PTX_t              RCL_CmdNesbPtx;
typedef struct RCL_CMD_NESB_PRX_t              RCL_CmdNesbPrx;
typedef struct RCL_STATS_NESB_t                RCL_StatsNesb;
typedef struct RCL_CONFIG_ADDRESS_t            RCL_ConfigAddress;

/* Command IDs for generic commands */
#define RCL_CMDID_GENERIC_FS            0x0001U
#define RCL_CMDID_GENERIC_FS_OFF        0x0002U
#define RCL_CMDID_GENERIC_TX            0x0003U
#define RCL_CMDID_GENERIC_TX_REPEAT     0x0004U
#define RCL_CMDID_GENERIC_TX_TEST       0x0005U
#define RCL_CMDID_GENERIC_RX            0x0006U
#define RCL_CMDID_GENERIC_LRF_OPERATION 0x0007U
#define RCL_CMDID_NESB_PTX              0x0008U
#define RCL_CMDID_NESB_PRX              0x0009U
#define RCL_CMDID_GENERIC_TX_BURST      0x000DU
#define RCL_CMDID_GENERIC_RX_BURST      0x000EU
#define RCL_CMDID_GENERIC_TX_STREAM     0x000FU
#define RCL_CMDID_GENERIC_RX_STREAM     0x0010U


/**
 *  @brief RF frequency programming type object
 *
 *  Type to specify how frequency programming is done for FS command
 */
typedef enum RCL_FsType_e {
    RCL_FsType_Rx,               /*!< Program synth as for RX operation */
    RCL_FsType_Tx,               /*!< Program synth as for TX operation */
} RCL_FsType;

/**
 *  @brief Frequency programming command
 *
 *  Command to program a synth frequency without directly starting RX or TX
 */
struct RCL_CMD_GENERIC_FS_t {
    RCL_Command     common;
    uint32_t        rfFrequency; /*!< RF frequency in Hz to program */
    RCL_FsType      fsType;      /*!< Rules for synth setup */
};

#define RCL_CmdGenericFs_Default()                          \
{                                                           \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_FS,     \
                                  RCL_Handler_Generic_Fs),  \
    .rfFrequency = 2440000000U,                             \
    .fsType = RCL_FsType_Rx,                                \
}
#define RCL_CmdGenericFs_DefaultRuntime() (RCL_CmdGenericFs) RCL_CmdGenericFs_Default()

/**
 *  @brief Stop frequency synthesizer command
 *
 *  Command to stop the frequency synthesizer if it is running after a command
 */
struct RCL_CMD_GENERIC_FS_OFF_t {
    RCL_Command     common;
};

#define RCL_CmdGenericFsOff_Default()                           \
{                                                               \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_FS_OFF,     \
                                  RCL_Handler_Generic_FsOff),   \
}
#define RCL_CmdGenericFsOff_DefaultRuntime() (RCL_CmdGenericFsOff) RCL_CmdGenericFsOff_Default()


/**
 *  @brief Generic transmit command
 *
 *  Command to transmit a packet
 */
struct RCL_CMD_GENERIC_TX_t {
    RCL_Command     common;
    uint32_t        rfFrequency; /*!< RF frequency in Hz to program. 0: Do not program frequency */
    List_List       txBuffers;   /*!< Linked list of packets to transmit. RCL will pop the first packet when transmitted. */
    uint32_t        syncWord;    /*!< Sync word to transmit */
    RCL_Command_TxPower txPower; /*!< Transmit power */
    struct {
        uint8_t     fsOff: 1;    /*!< 0: Keep PLL enabled after command. 1: Turn off FS after command. */
        uint8_t     reserved: 7; /*!< Reserved, set to 0 */
    } config;
};
#define RCL_CmdGenericTx_Default()                          \
{                                                           \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_TX,     \
                                  RCL_Handler_Generic_Tx),  \
    .rfFrequency = 2440000000U,                             \
    .txBuffers = { 0 },                                     \
    .syncWord = 0x930B51DE,                                 \
    .txPower = {.dBm = 0, .fraction = 0},                   \
    .config = {                                             \
        .fsOff = 1,                                         \
        .reserved = 0,                                      \
    },                                                      \
}
#define RCL_CmdGenericTx_DefaultRuntime() (RCL_CmdGenericTx) RCL_CmdGenericTx_Default()

/**
 *  @brief Generic repeated packet transmit command
 *
 *  Command to transmit a packet repeatedly
 */
struct RCL_CMD_GENERIC_TX_REPEAT_t {
    RCL_Command          common;
    uint32_t             rfFrequency; /*!< RF frequency in Hz to program. 0: Do not program frequency */
    RCL_Buffer_DataEntry *txEntry;    /*!< Packet to transmit */
    uint32_t             syncWord;    /*!< Sync word to transmit */
    uint32_t             timePeriod;  /*!< Time period (0.25 us units) of repeated transmissions. 0: Back-to-back */
    uint16_t             numPackets;  /*!< Number of times to send the packet: 0: Unlimited */
    RCL_Command_TxPower  txPower;     /*!< Transmit power */
    struct {
        uint8_t          fsOff: 1;    /*!< 0: Keep PLL enabled after command. 1: Turn off FS after command. */
        uint8_t          fsRecal: 1;  /*!< 0: Keep synth running between each packet. 1. Turn off synth after each packet and recalibrate for the next. Requires %rfFrequency != 0 */
        uint8_t          reserved: 6; /*!< Reserved, set to 0 */
    } config;
};
#define RCL_CmdGenericTxRepeat_Default()                        \
{                                                               \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_TX_REPEAT,  \
                                  RCL_Handler_Generic_TxRepeat),\
    .rfFrequency = 2440000000U,                                 \
    .txEntry = NULL,                                            \
    .syncWord = 0x930B51DE,                                     \
    .timePeriod = 0,                                            \
    .numPackets = 0,                                            \
    .config = {                                                 \
        .fsOff = 1,                                             \
        .fsRecal = 0,                                           \
        .reserved = 0,                                          \
    },                                                          \
}
#define RCL_CmdGenericTxRepeat_DefaultRuntime() (RCL_CmdGenericTxRepeat) RCL_CmdGenericTxRepeat_Default()

/**
 *  @brief Generic transmitter test command
 *
 *  Command to transmit continuously, either a modulated signal or continuous wave
 */
struct RCL_CMD_GENERIC_TX_TEST_t {
    RCL_Command     common;
    uint32_t        rfFrequency;   /*!< RF frequency in Hz to program. 0: Do not program frequency */
    RCL_Command_TxPower txPower;   /*!< Transmit power */
    struct {
        uint32_t     txWord: 16;   /*!< Repeated word to transmit */
        uint32_t     whitenMode: 2;/*!< 0. No whitening. 1: Default whitening. 2: PRBS-15. 3: PRBS-32 */
        uint32_t     sendCw: 1;    /*!< 0: Send modulated signal. 1: Send CW */
        uint32_t     fsOff: 1;     /*!< 0: Keep PLL enabled after command. 1: Turn off FS after command. */
        uint32_t     reserved: 12; /*!< Reserved, set to 0 */
    } config;
};
#define RCL_CmdGenericTxTest_Default()                          \
{                                                               \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_TX_TEST,    \
                                  RCL_Handler_Generic_TxTest),  \
    .rfFrequency = 2440000000U,                                 \
    .txPower = {.dBm = 0, .fraction = 0},                       \
    .config = {                                                 \
        .txWord = 0,                                            \
        .whitenMode = 2,                                        \
        .sendCw = 0,                                            \
        .fsOff = 1,                                             \
        .reserved = 0,                                          \
    },                                                          \
}
#define RCL_CmdGenericTxTest_DefaultRuntime() (RCL_CmdGenericTxTest) RCL_CmdGenericTxTest_Default()

#define RCL_CMD_GENERIC_WH_MODE_NONE     0U /*!< config.whitenMode: No whitening */
#define RCL_CMD_GENERIC_WH_MODE_DEFAULT  1U /*!< config.whitenMode: Default whitening */
#define RCL_CMD_GENERIC_WH_MODE_PRBS15   2U /*!< config.whitenMode: PRBS-15 */
#define RCL_CMD_GENERIC_WH_MODE_PRBS32   3U /*!< config.whitenMode: PRBS-32 */

/**
 *  @brief Generic receive command
 *
 *  Command to receive a packet
 */
struct RCL_CMD_GENERIC_RX_t {
    RCL_Command     common;
    uint32_t        rfFrequency;         /*!< RF frequency in Hz to program. 0: Do not program frequency */
    List_List       rxBuffers;           /*!< Linked list of buffers where packets are stored */
    RCL_StatsGeneric *stats;             /*!< Pointer to statistics structure. NULL: Do not store statistics */
    uint32_t        syncWordA;           /*!< Sync word to listen for */
    uint32_t        syncWordB;           /*!< Alternate  Sync word to listen for */
    uint16_t        maxPktLen;           /*!< Maximum packet length, or packet length for fixed length */
    struct {
        uint8_t     repeated: 1;         /*!< 0: End after receiving one packet. 1: Go back to sync search after receiving. */
        uint8_t     disableSyncA: 1;     /*!< 0: Listen for syncWordA. 1: Do not listen for syncWordA */
        uint8_t     disableSyncB: 1;     /*!< 0: Listen for syncWordB. 1: Do not listen for syncWordB */
        uint8_t     discardRxPackets: 1; /*!< 0: Store received packets in rxBuffers. 1: Do not store packets, useful for link tests where checksum result is enough */
        uint8_t     fsOff: 1;            /*!< 0: Keep PLL enabled after command. 1: Turn off FS after command. */
        uint8_t     reserved: 3;         /*!< Reserved, set to 0 */
    } config;
};
#define RCL_CmdGenericRx_Default()                          \
{                                                           \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_RX,     \
                                  RCL_Handler_Generic_Rx),  \
    .rfFrequency = 2440000000U,                             \
    .rxBuffers = {0},                                       \
    .stats = NULL,                                          \
    .syncWordA = 0x930B51DE,                                \
    .syncWordB = 0x12345678,                                \
    .maxPktLen = 255,                                       \
    .config = {                                             \
        .repeated = 1,                                      \
        .disableSyncA = 0,                                  \
        .disableSyncB = 1,                                  \
        .discardRxPackets = 0,                              \
        .fsOff = 1,                                         \
        .reserved = 0,                                      \
    },                                                      \
}
#define RCL_CmdGenericRx_DefaultRuntime() (RCL_CmdGenericRx) RCL_CmdGenericRx_Default()

struct RCL_STATS_GENERIC_t {
    struct
    {
        uint8_t accumulate : 1;      /*!< 0: Reset counters to 0 at start of command. 1: Add to incoming value of counters. */
        uint8_t activeUpdate : 1;    /*!< 0: Update only at end of command. 1: Update after receiving or transmitting packets. */
        uint8_t reserved : 6;        /*!< Reserved, set to 0 */
    } config;                        /*!< Configuration provided to RCL */
    uint8_t   timestampValid;        /*!< Returns 1 if %lastTimestamp is updated; 0 otherwise */
    int8_t    lastRssi;              /*!< RSSI of last received packet. */
    uint32_t  lastTimestamp;         /*!< Timestamp of last successfully received packet */
    uint32_t  nRxNok;                /*!< Number of packets received with CRC error */
    uint32_t  nRxOk;                 /*!< Number of correctly received packets */
};

#define RCL_StatsGeneric_Default()  \
{                                   \
    .config = { 0 },                \
    .timestampValid = 0,            \
    .lastRssi = LRF_RSSI_INVALID,   \
}
#define RCL_StatsGeneric_DefaultRuntime() (RCL_StatsGeneric) RCL_StatsGeneric_Default()

/**
 *  @brief Send LRF operation
 *
 *  Send an opcode to the LRF and wait for it to report done
 */
struct RCL_CMD_GENERIC_LRF_OPERATION_t {
    RCL_Command     common;
    uint16_t        lrfOperation;     /*!<  Operation code to send to the LRF */
};

#define RCL_CmdGenericLrfOperation_Default()                        \
{                                                                   \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_LRF_OPERATION,  \
                                  RCL_Handler_Generic_LrfOperation),\
    .lrfOperation = LRF_INTERFACE_GENERIC_API_OP_PING,              \
}
#define RCL_CmdGenericLrfOperation_DefaultRuntime() (RCL_CmdGenericLrfOperation) RCL_CmdGenericLrfOperation_Default()

/**
 *  @brief NESB transmit command
 *
 *  Command to transmit a packet
 */
struct RCL_CMD_NESB_PTX_t {
    RCL_Command     common;
    uint32_t        rfFrequency;            /*!< RF frequency in Hz to program. 0: Do not program frequency. */
    List_List       txBuffers;              /*!< Linked list of packets to transmit. RCL will pop the first packet when transmitted. */
    List_List       rxBuffers;              /*!< Linked list of buffers for storing received packets. In this case, the ACK. */
    RCL_StatsNesb   *stats;                /*!< Pointer to statistics structure. NULL: Do not store statistics */
    uint32_t        syncWord;               /*!< Sync word to transmit */
    RCL_Command_TxPower txPower;            /*!< Transmit power */
    uint8_t         seqNo;                  /*!< Sequence number to use for next packet */
    uint8_t         maxRetrans;             /*!< Maximum number of retransmissions */
    uint32_t        retransDelay;           /*!< Number of Systim ticks (250 [ns] resolution) from start of transmission of a packet to retransmission.
                                                 If an unattainable retransmission delay is set, the retransmission will start as soon as possible. */
    struct {
        uint8_t     fsOff: 1;               /*!< 0: Keep PLL enabled after command. 1: Turn off FS after command. */
        uint8_t     autoRetransmitMode: 2;  /*!< 0: Do not listen for ACK.
                                                 1: Listen for ACK if transmitted NO_ACK = 0 and retransmit if missing.
                                                 2: Listen for ACK if transmitted NO_ACK = 1 and retransmit if missing.
                                                 3: Always listen for ACK and retransmit if missing. */
        uint8_t     hdrConf: 1;             /*!< 0: Insert NO_ACK field from TX buffer.
                                                 1: Insert SEQ and NO_ACK field from TX buffer. */
        uint8_t     reserved: 4;            /*!< Reserved, set to 0 */
    } config;
};
#define RCL_CmdNesbPtx_Default()                                   \
{                                                                  \
    .common = RCL_Command_Default(RCL_CMDID_NESB_PTX,              \
                                  RCL_Handler_Nesb_Ptx),           \
    .rfFrequency = 2440000000U,                                    \
    .txBuffers = { 0 },                                            \
    .rxBuffers = { 0 },                                            \
    .stats = NULL,                                                 \
    .syncWord = 0x930B51DE,                                        \
    .txPower = {.dBm = 0, .fraction = 0},                          \
    .seqNo = 0,                                                    \
    .maxRetrans = 5,                                               \
    .retransDelay = 100000,                                        \
    .config = {                                                    \
        .fsOff = 1,                                                \
        .autoRetransmitMode = 3,                                   \
        .hdrConf = 1,                                              \
        .reserved = 0,                                             \
    }                                                              \
}
#define RCL_CmdNesbPtx_DefaultRuntime() (RCL_CmdNesbPtx) RCL_CmdNesbPtx_Default()

struct RCL_CONFIG_ADDRESS_t {
    uint32_t    address;                /*!< Address after header */
    uint16_t    crcVal;                 /*!< CRC value (last two bytes if more than 2 CRC bytes) of last successfully received
                                             packet. */
    uint8_t     maxPktLen;              /*!< Packet length for fixed length, maximum packet length for variable
                                             length */
    uint8_t     autoAckMode: 2;         /*!< 0: Disable auto-acknowledgement.
                                             1: Enable auto-acknowledgement if received NO_ACK = 0.
                                             2: Enable auto-acknowledgement if received NO_ACK = 1.
                                             3: Enable auto-acknowledgement regardless of received NO_ACK. */
    uint8_t     varLen: 1;              /*!< 0: Use fixed length given by maxPktLenA in receiver when receiving packets
                                             1: Use variable length in receiver when receiving packets */
    uint8_t     seqValid: 1;            /*!< 0: The status is not valid. Any packet is viewed as new.
                                             1: The status is valid. Only packets with sequence number and CRC different from
                                                the previous one are accepted. */
    uint8_t     seq: 2;                 /*!< Sequence number of last successfully received packet */
    uint8_t     reserved: 2;            /*!< Reserved, set to 0  */
};

#define RCL_ConfigAddress_Default(_addr)       \
{                                              \
    .address = _addr,                          \
    .crcVal = 0,                               \
    .maxPktLen = 255,                          \
    .autoAckMode = 3,                          \
    .varLen = 1,                               \
    .seqValid = 0,                             \
    .seq = 1,                                  \
    .reserved = 0,                             \
}
#define RCL_ConfigAddress_DefaultRuntime(_addr) (RCL_ConfigAddress) RCL_ConfigAddress_Default(_addr)


/**
 *  @brief NESB receive command
 *
 *  Command to receive a packet
 */
struct RCL_CMD_NESB_PRX_t {
    RCL_Command     common;
    uint32_t        rfFrequency;            /*!< RF frequency in Hz to program. 0: Do not program frequency */
    List_List       rxBuffers;              /*!< Linked list of buffers where packets are stored */
    RCL_StatsNesb   *stats;                /*!< Pointer to statistics structure. NULL: Do not store statistics */
    uint32_t        syncWordA;              /*!< Sync word to listen for */
    uint32_t        syncWordB;              /*!< Alternate Sync word to listen for */
    RCL_Command_TxPower txPower;            /*!< Transmit power for ACKs */
    uint8_t         addrLen;                /*!< Length of address after header (0-4 bytes) */
    struct {
        uint8_t     disableSyncA: 1;        /*!< 0: Listen for syncWordA. 1: Do not listen for syncWordA */
        uint8_t     disableSyncB: 1;        /*!< 0: Listen for syncWordB. 1: Do not listen for syncWordB */
        uint8_t     discardRxPackets: 1;    /*!< 0: Store received packets in rxBuffers.
                                                 1: Do not store packets, useful for link tests where checksum result is enough */
        uint8_t     fsOff: 1;               /*!< 0: Keep PLL enabled after command. 1: Turn off FS after command. */
        uint8_t     repeatOk: 1;            /*!< 0: End operation after receiving a packet correctly.
                                                 1: Go back to sync search after receiving a packet correctly */
        uint8_t     repeatNok: 1;           /*!< 0: End operation after receiving a packet with CRC error or address mismatch.
                                                 1: Go back to sync search after receiving a packet with CRC error or address
                                                    mismatch */
        uint8_t     reserved: 2;            /*!< Reserved, set to 0 */
    } config;
    union {
        RCL_ConfigAddress syncWord[2];
        struct {
            RCL_ConfigAddress syncWordACfg;
            RCL_ConfigAddress syncWordBCfg;
        };
    };
};
#define RCL_CmdNesbPrx_Default()                                   \
{                                                                  \
    .common = RCL_Command_Default(RCL_CMDID_NESB_PRX,              \
                                  RCL_Handler_Nesb_Prx),           \
    .rfFrequency = 2440000000U,                                    \
    .rxBuffers = {0},                                              \
    .stats = NULL,                                                 \
    .syncWordA = 0x930B51DE,                                       \
    .syncWordB = 0x570451AE,                                       \
    .txPower = {.dBm = 0, .fraction = 0},                          \
    .addrLen = 4,                                                  \
    .config = {                                                    \
        .disableSyncA = 0,                                         \
        .disableSyncB = 1,                                         \
        .discardRxPackets = 0,                                     \
        .fsOff = 1,                                                \
        .repeatOk = 1,                                             \
        .repeatNok = 0,                                            \
        .reserved = 0,                                             \
    },                                                             \
    .syncWord[0] = RCL_ConfigAddress_Default(0xEFFEABBA),          \
    .syncWord[1] = RCL_ConfigAddress_Default(0xEFFEABBC)           \
}
#define RCL_CmdNesbPrx_DefaultRuntime() (RCL_CmdNesbPrx) RCL_CmdNesbPrx_Default()

struct RCL_STATS_NESB_t {
    struct
    {
        uint8_t accumulate : 1;      /*!< 0: Reset counters to 0 at start of command. 1: Add to incoming value of counters. */
        uint8_t activeUpdate : 1;    /*!< 0: Update only at end of command. 1: Update after receiving or transmitting packets. */
        uint8_t reserved : 6;        /*!< Reserved, set to 0 */
    } config;                        /*!< Configuration provided to RCL */
    uint8_t   timestampValid;        /*!< Returns 1 if %lastTimestamp is updated; 0 otherwise */
    int8_t    lastRssi;              /*!< RSSI of last received packet. */
    uint32_t  lastTimestamp;         /*!< Timestamp of last successfully received packet */
    uint32_t  nTx;                   /*!< Number of packets or acknowledgements transmitted */
    uint32_t  nRxNok;                /*!< Number of packets that have been received with CRC error */
    uint32_t  nRxOk;                 /*!< Number of packets that have been received with CRC OK and not ignored */
    uint32_t  nRxIgnored;            /*!< Number of packets ignored as retransmissions */
    uint32_t  nRxAddrMismatch;       /*!< Number of packets ignored due to address mismatch */
    uint32_t  nRxBufFull;            /*!< Number of packets that have been received and discarded due to lack of buffer space */
};

#define RCL_StatsNesb_Default()  \
{                                   \
    .config = { 0 },                \
    .timestampValid = 0,            \
    .lastRssi = -128,               \
}
#define RCL_StatsNesb_DefaultRuntime() (RCL_StatsNesb) RCL_StatsNesb_Default()

/*!
 *  @brief Per-slot outcome after an RX burst completes
 */
typedef enum {
    RCL_RX_BURST_SLOT_STATUS_PENDING  = 0, /*!< Slot not yet processed */
    RCL_RX_BURST_SLOT_STATUS_OK       = 1, /*!< Packet received with correct CRC */
    RCL_RX_BURST_SLOT_STATUS_CRC_FAIL = 2, /*!< Packet received but CRC error */
    RCL_RX_BURST_SLOT_STATUS_TIMEOUT  = 3  /*!< No packet arrived within slot window */
} RCL_RxBurstSlotStatus;

/**
 *  @brief RX burst command
 *
 *  Receives up to %numPackets packets back to back. The PBE re-arms sync
 *  search after each packet on its own and the LRF DMA moves each received
 *  entry out of the RX FIFO into the buffer posted with RCL_Dma_putRxBuffer()
 *  as the PBE commits it, so the CPU is idle for the entire burst. The
 *  inter-packet gap comes from the RF settings (PBE PRERXIFS), not from this
 *  command.
 *
 *  The PBE ends the burst itself once %numPackets packets have arrived, good
 *  or bad. A burst that loses a packet never reaches that count and ends on a
 *  sync-search timeout instead (%firstPktTimeoutTicks for the first packet,
 *  %slotTimeoutTicks for the following ones), or on the stop time in
 *  %common.timing, whichever comes first.
 *
 *  @note CC27XX only. Other devices end the command with RCL_CommandStatus_Error_Param.
 */
struct RCL_CMD_GENERIC_RX_BURST_t {
    RCL_Command             common;
    uint32_t                rfFrequency;            /*!< RF frequency in Hz. 0: do not program frequency */
    uint32_t                syncWord;               /*!< Sync word to match */
    uint16_t                numPackets;             /*!< Packets in the burst. Sizes the %slotStatus array and the DMA transfer; the burst need not fit the RX FIFO, see RCL_Dma_armRxBurst */
    uint16_t                packetLength;           /*!< Payload bytes per packet */
    uint32_t                firstPktTimeoutTicks;   /*!< Max ticks to wait for first packet. 0: wait forever */
    uint32_t                slotTimeoutTicks;       /*!< Max ticks to wait for each subsequent packet's sync. 0: wait forever. On timeout the burst ends on that slot and the remaining slots are marked TIMEOUT */
    RCL_RxBurstSlotStatus  *slotStatus;             /*!< Caller-provided array [numPackets] for per-slot outcome */
    RCL_StatsGenericRxBurst *stats;                 /*!< Pointer to statistics structure. NULL: Do not store statistics */
    struct {
        uint8_t             fsOff:     1;           /*!< 0: keep PLL after command. 1: turn off FS after command */
        uint8_t             enableLRF: 1;           /*!< 0: assume the LRF is already enabled by a previous command (chained bursts). 1: call LRF_enable() at command start */
        uint8_t             disableLRF:1;           /*!< 0: leave the LRF enabled for a following chained command. 1: call LRF_disable() at command end */
        uint8_t             reconfigure:1;          /*!< 0: reuse the previous burst's static config and reprogram only the frequency. 1: program the static config (sync word, OPCFG, timeouts, packet length, packet count). */
        uint8_t             reserved:  4;           /*!< Reserved, set to 0 */
    } config;
};

#define RCL_CmdGenericRxBurst_Default()                                         \
{                                                                               \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_RX_BURST,                   \
                                  RCL_Handler_Generic_RxBurst),                 \
    .rfFrequency          = 2440000000U,                                        \
    .syncWord             = 0x930B51DEU,                                        \
    .numPackets           = 5U,                                                 \
    .packetLength         = 22U,                                                \
    .firstPktTimeoutTicks = 0U,                                                 \
    .slotTimeoutTicks     = 0U,                                                 \
    .slotStatus           = NULL,                                               \
    .stats                = NULL,                                               \
    .config = { .fsOff = 0, .enableLRF = 1, .disableLRF = 1,                    \
                .reconfigure = 1, .reserved = 0 },                              \
}
#define RCL_CmdGenericRxBurst_DefaultRuntime() \
    (RCL_CmdGenericRxBurst) RCL_CmdGenericRxBurst_Default()

/**
 *  @brief Multi-packet TX burst command
 *
 *  Transmits %numPackets distinct packets back to back, fed one packet at a
 *  time by the LRF DMA as the PBE asks for it, from the entries posted with
 *  RCL_Dma_putTxBurst(); an entry may be written up to the moment the FIFO
 *  takes it. The CPU is idle for the burst; the inter-packet gap comes from
 *  the RF settings (PBE PRETXIFS), not from this command. The PBE ends the
 *  burst itself once it has sent %numPackets packets.
 *
 *  @note CC27XX only. Other devices end the command with RCL_CommandStatus_Error_Param.
 */
struct RCL_CMD_GENERIC_TX_BURST_t {
    RCL_Command          common;
    uint32_t             rfFrequency;     /*!< RF frequency in Hz to program. 0: Do not program frequency */
    uint32_t             syncWord;        /*!< Sync word to transmit */
    uint16_t             numPackets;      /*!< Number of packets to transmit */
    uint16_t             packetLength;    /*!< Payload bytes per packet */
    RCL_Command_TxPower  txPower;         /*!< Transmit power */
    struct {
        uint8_t          fsOff: 1;        /*!< 0: Keep PLL enabled after command. 1: Turn off FS after command. */
        uint8_t          enableLRF: 1;    /*!< 1: call LRF_enable() at command start. 0: assume the LRF is already enabled by a previous command (chained bursts) */
        uint8_t          disableLRF: 1;   /*!< 1: call LRF_disable() at command end. 0: leave the LRF enabled for a following chained command */
        uint8_t          reconfigure: 1;  /*!< 1: program the static config (sync word, TX power, OPCFG, packet length, packet count). 0: reuse the previous burst's static config and reprogram only the frequency; %syncWord, %packetLength, %numPackets and %txPower must then match the previous burst */
        uint8_t          reserved: 4;     /*!< Reserved, set to 0 */
    } config;
    RCL_StatsGenericTxBurst *stats;       /*!< Pointer to statistics structure. NULL: Do not store statistics */
};
#define RCL_CmdGenericTxBurst_Default()                          \
{                                                                 \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_TX_BURST,    \
                                  RCL_Handler_Generic_TxBurst),  \
    .rfFrequency = 2440000000U,                                  \
    .syncWord = 0x930B51DE,                                      \
    .numPackets = 5,                                             \
    .packetLength = 22,                                          \
    .txPower = {.dBm = 0, .fraction = 0},                        \
    .config = {                                                  \
        .fsOff = 0,                                              \
        .enableLRF = 1,                                          \
        .disableLRF = 1,                                         \
        .reconfigure = 1,                                        \
        .reserved = 0,                                           \
    },                                                           \
    .stats = NULL,                                               \
}
#define RCL_CmdGenericTxBurst_DefaultRuntime() (RCL_CmdGenericTxBurst) RCL_CmdGenericTxBurst_Default()

struct RCL_STATS_GENERIC_TX_BURST_t {
    struct
    {
        uint8_t accumulate : 1;  /*!< 0: Reset counter to 0 at start of command. 1: Add to incoming value of counter. */
        uint8_t reserved : 7;    /*!< Reserved, set to 0 */
    } config;                    /*!< Configuration provided to RCL */
    uint16_t nTx;                /*!< Number of packets transmitted */
};

#define RCL_StatsGenericTxBurst_Default() \
{                                         \
    .config = { 0 },                      \
    .nTx = 0,                             \
}
#define RCL_StatsGenericTxBurst_DefaultRuntime() (RCL_StatsGenericTxBurst) RCL_StatsGenericTxBurst_Default()

struct RCL_STATS_GENERIC_RX_BURST_t {
    struct
    {
        uint8_t accumulate : 1;  /*!< 0: Reset counter to 0 at start of command. 1: Add to incoming value of counter. */
        uint8_t reserved : 7;    /*!< Reserved, set to 0 */
    } config;                    /*!< Configuration provided to RCL */
    uint16_t nRxOk;              /*!< Number of packets received with correct CRC */
};

#define RCL_StatsGenericRxBurst_Default() \
{                                         \
    .config = { 0 },                      \
    .nRxOk = 0,                           \
}
#define RCL_StatsGenericRxBurst_DefaultRuntime() (RCL_StatsGenericRxBurst) RCL_StatsGenericRxBurst_Default()

/**
 *  @brief TX stream command
 *
 *  Holds the radio up and configured for transmission and never posts an
 *  operation of its own. Every packet is one single-packet PBE operation
 *  that the application, or a DMA task working for it, starts by pushing an
 *  entry into the TX FIFO through the LRFDTXF data port and writing OP_TX to
 *  LRFDPBE.API; OPCFG.START is asynchronous, so the operation starts on that
 *  write and not on a SysTimer compare, and the packet is on the air a fixed
 *  time after the write. The CPU is not involved per packet: only the end of
 *  the command is serviced.
 *
 *  The FIFO is prepared once and issues no command afterwards. FCFG0.TXACOM
 *  commits every pushed word and FCFG0.TXADEAL frees a packet as the
 *  modulator consumes it, so neither the PBE (OPCFG.TXFCMD is NONE) nor the
 *  CPU writes LRFDPBE.FCMD while the command runs. That is what keeps RCL-367
 *  off the transmit path: the defect needs a FIFO command and a data port
 *  access in consecutive cycles, and there is no FIFO command.
 *
 *  With %rfFrequency set the command programs the frequency and runs one
 *  synthesizer calibration of its own at setup, on the command's start time,
 *  and only reports %RCL_EventCmdStarted once it has locked; the packet
 *  operations then run without calibration on a synthesizer the same TOPSM
 *  state locked, about 9 us from the API write to the first bit. Post the
 *  first operation only after that event: before it the PBE is calibrating,
 *  or holds the command's start compare, which it takes as the hard stop of
 *  a running operation. Post the next only once the previous operation has
 *  ended: the PBE rejects an operation written while one is running as a bad
 *  operation, which ends the command.
 *
 *  With %rfFrequency 0 the command trusts the synthesizer to be locked, as
 *  after an FS command, and calibrates nothing. That path is not
 *  recommended: the FS command ends with FS_KEEPON off and powers the LRF
 *  down, this command powers it up again and re-initialises the RFE, and
 *  what the RFE then transmits with rests on calibration state the
 *  re-initialisation did not redo, while the lock flag the check reads is
 *  RFE firmware state that survives it. Measured: one CC2755P20 transmitted
 *  correctly this way and another transmitted nothing a receiver could sync
 *  to, with the same code and the lock flag set on both.
 *
 *  The command stays active until it is stopped, through the API or by a
 *  stop time, or the PBE reports an operation error. A graceful stop lets an
 *  operation in flight finish; a hard stop ends it. Stop posting before
 *  stopping the command: an operation posted after the stop is requested is
 *  not accounted for, and a hard stop that lands in the first microseconds
 *  of an operation, before the RFE has brought the PA up (about 9 us after
 *  the API write), leaves the PBE waiting in its end routine for an RFE
 *  report that never comes, with no recovery short of resetting the LRF;
 *  measured. Prefer the graceful stop. Leave %config.disableLRF set: a stop
 *  can leave a second operation done latched in the doorbell, which a
 *  following command on an LRF left enabled would take for its own.
 *
 *  Frequency hopping. With %hopFrequencies set the command hops through the
 *  table's channels, %packetsPerHop packets on each, in table order and
 *  wrapping, starting on the first. At setup, with the front end idle, it
 *  programs every channel of the table and keeps what LRF_programFrequency()
 *  wrote for each as a row of %RCL_GENERIC_HOP_ROW_WORDS words in %hopRows:
 *  the RFE RAM frequency and IF words, the coarse and mid calibration
 *  dividers, the two PLL words, the demodulator resampler fraction and its
 *  shadow, the mixer word and its copy, the six TX filter tap words and the
 *  shaping gain. The rows are values of this device with the HFXT
 *  compensation of the moment, captured at setup, never constants; if the
 *  compensation changes while the command runs the rows are stale, and the
 *  command is to be submitted again to capture them anew. The setup grows
 *  with %numHops, one LRF_programFrequency() and a row read per channel.
 *  The programming ends on the first channel, the calibration of the setup
 *  runs there, and the TX power is programmed for that channel; the table
 *  is to stay within one band. %rfFrequency is not used. The PBE is then
 *  told, through NTXIRQ, a word of the patched generic image, to raise an
 *  interrupt after every %packetsPerHop-th packet, and on that interrupt the
 *  handler moves to the next channel and writes its row: the stores of the
 *  row, nothing computed. The RFE is idle when the interrupt is raised:
 *  the PBE's TX_DONE waits for the modem's and then the RFE's report before
 *  the NTX store the interrupt is raised from, so the RFE's report of the
 *  operation is in the PBE's RFEMSGBOX by then (measured as well: 3 us
 *  after the PA has gone down and 4 us before the operation done). The row
 *  is written only if that report is still there. The RFE clears the word
 *  when it takes its next command, about a microsecond after the
 *  application's API write, so a hop that finds it cleared, because the
 *  application posted the next operation first or the RCL's interrupt was
 *  held off until it did, skips the row and is counted in
 *  %stats.nHopsMissed, and the channel moves on regardless, so a late hop
 *  costs one dwell on the wrong channel and not the front end. A post made
 *  in the microsecond before the RFE has taken it up is not seen; that
 *  window is the application's contract: the first operation of a dwell is
 *  posted after the previous dwell's last operation has ended, as any
 *  operation is, and the RCL's interrupt is not held off across that gap.
 *  The application may see the hop itself by subscribing to doorbell bit 9,
 *  %LRF_EventTxCtrl, in its lrfCallbackMask. The synthesizer stays on and
 *  locked through the hop, and the first packet on the new channel goes
 *  out like any other. When the application counts its packets from 0 at
 *  the first one it posts, the channel of packet n is (n / packetsPerHop)
 *  mod numHops, which is what a hopping RX stream counts on. NTX restarts
 *  from zero at every hop, so %stats.nTx is recomputed from the hops, and
 *  %stats.nHops counts the hops made. Stops are as without hopping. NTXIRQ
 *  is written back to zero when the command ends. It is zero unless a
 *  stream sets it, every transmit command zeroes it at setup, and NTXTARGET
 *  can only end a burst if it is smaller than NTXIRQ, so the burst commands
 *  are unaffected by it. A table without rows, with fewer than two
 *  channels, with a zero frequency in it or with %packetsPerHop zero ends
 *  the command with RCL_CommandStatus_Error_Param.
 *
 *  @note CC27XX only. Other devices end the command with RCL_CommandStatus_Error_Param.
 */
struct RCL_CMD_GENERIC_TX_STREAM_t {
    RCL_Command          common;
    uint32_t             rfFrequency;     /*!< RF frequency in Hz to program. 0: Do not program frequency */
    uint32_t             syncWord;        /*!< Sync word to transmit */
    uint16_t             packetLength;    /*!< Payload bytes per packet */
    RCL_Command_TxPower  txPower;         /*!< Transmit power */
    struct {
        uint8_t          fsOff: 1;        /*!< 0: keep refsys and the power constraints after the command, for a following command on the locked synthesizer. 1: release them */
        uint8_t          enableLRF: 1;    /*!< 1: call LRF_enable() at command start. 0: assume the LRF is already enabled by a previous command */
        uint8_t          disableLRF: 1;   /*!< 1: call LRF_disable() at command end. 0: leave the LRF enabled for a following command; not supported after a stop, see above */
        uint8_t          reserved: 5;     /*!< Reserved, set to 0 */
    } config;
    RCL_StatsGenericTxStream *stats;      /*!< Pointer to statistics structure. NULL: Do not store statistics */
    const uint32_t      *hopFrequencies;  /*!< Frequency hop table in Hz, taken in order. NULL: no hopping */
    uint32_t            *hopRows;         /*!< With a table: %numHops * %RCL_GENERIC_HOP_ROW_WORDS words the handler fills at setup and reads at every hop; must live for the life of the command */
    uint8_t              numHops;         /*!< Channels in the table, 2 or more */
    uint8_t              packetsPerHop;   /*!< Packets sent on a channel before the hop, 1 or more */
};
#define RCL_CmdGenericTxStream_Default()                          \
{                                                                  \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_TX_STREAM,    \
                                  RCL_Handler_Generic_TxStream),  \
    .rfFrequency = 2440000000U,                                   \
    .syncWord = 0x930B51DE,                                       \
    .packetLength = 24,                                           \
    .txPower = {.dBm = 0, .fraction = 0},                         \
    .config = {                                                   \
        .fsOff = 0,                                               \
        .enableLRF = 1,                                           \
        .disableLRF = 1,                                          \
        .reserved = 0,                                            \
    },                                                            \
    .stats = NULL,                                                \
    .hopFrequencies = NULL,                                       \
    .hopRows = NULL,                                              \
    .numHops = 0,                                                 \
    .packetsPerHop = 0,                                           \
}
#define RCL_CmdGenericTxStream_DefaultRuntime() (RCL_CmdGenericTxStream) RCL_CmdGenericTxStream_Default()

/**
 *  @brief RX stream command
 *
 *  Runs one repeated RX operation for as long as the command is active: the
 *  PBE re-arms sync search after every packet on its own, without timeouts,
 *  and commits each received entry to the RX FIFO. The command routes the RX
 *  FIFO commit to the LRF DMA trigger, so the application's DMA channel gets
 *  one request per committed entry and takes the entry out through
 *  LRFDPBE.RXFHRD; the command itself never touches a DMA channel. The
 *  appended status, RSSI, LQI, frequency estimate and timestamp bytes are
 *  turned off, so an entry is the length field, the pad and the payload and
 *  nothing else, and %entryBytes reports its size for the application to
 *  check against what it arms.
 *
 *  FCFG0.RXADEAL gives the FIFO space back as the entry is read, so no FIFO
 *  pointer is written while the operation is on the air. The CPU is not
 *  involved per packet: only the end of the command is serviced.
 *
 *  The drain is the application's, and it owes the command the following.
 *  The channel must be one the LRF trigger reaches, 2 or 4, subscribed to
 *  LRFDTRG; the request is a pulse per committed entry and one request is
 *  answered with one arbitration, so the transfer must be armed for exactly
 *  one entry of %entryBytes with an arbitration size, a power of two, that
 *  covers it, halfwords out of LRFDPBE.RXFHRD, and re-armed for every entry.
 *  It must be armed before the command starts: an entry nobody takes out
 *  stays in the FIFO, and once the FIFO is full the operation ends with
 *  %RCL_CommandStatus_Error_RxFifo. It must have taken an entry out before
 *  the next packet ends: the PBE issues its commit and discard FIFO commands
 *  at the end of every packet, and a port access in the cycle one of them
 *  takes effect drops the command. The trigger is taken away when the
 *  command ends and the channel is left as it was armed; the RCL's own
 *  channel may be used only while no burst command is queued, since a burst
 *  disables it when it ends.
 *
 *  The command stays active until it is stopped, through the API or by a
 *  stop time, or the PBE reports an operation error. The operation turns the
 *  synthesizer off when it ends, whatever %config.fsOff says: fsOff only
 *  decides whether refsys and the power constraints are kept for a following
 *  command, which must then program its frequency again. A hard stop in the
 *  first microseconds after the command starts, while the RFE is still
 *  starting the operation, leaves the PBE waiting in its end routine for an
 *  RFE report that never comes, as it does on the TX stream; prefer the
 *  graceful stop, which ends promptly in sync search. Leave
 *  %config.disableLRF set: a stop can leave a second operation done latched
 *  in the doorbell, which a following command on an LRF left enabled would
 *  take for its own.
 *
 *  Frequency hopping. With %hopFrequencies set the command receives on the
 *  table's channels in order, wrapping, one operation per dwell of
 *  %packetsPerHop packets, with the rows captured at setup as the TX stream
 *  captures them (its description lists them) and %rfFrequency unused. The
 *  first operation starts on the first channel with the setup's calibration
 *  and keeps the synthesizer on; every operation after it the handler posts
 *  with OPCFG.START asynchronous, no calibration and the synthesizer kept
 *  on, so that the synthesizer is neither turned off nor recalibrated for
 *  the life of the command and a hop costs about 14 us of LNA_EN low. An
 *  operation ends on a packet count, NRXTARGET, or on a sync-search timeout,
 *  and at every end the handler decides whether the dwell is over: if so it
 *  writes the next channel's row, with the PBE and the RFE idle, and posts;
 *  if not it posts again on the same channel for the packets still expected.
 *  The count is armed by RCL_CmdGenericRxStream_hopSync(), which the
 *  application calls from its per-packet callback with the transmitter's
 *  packet counter; that is what aligns the receiver's dwells with the
 *  transmitter's. Without it an operation ends only on the timeouts, which
 *  %packetPeriodTicks, the transmitter's packet period in 0.25 us ticks,
 *  sets: RXTIMEOUT is one period, so a packet one period late ends the
 *  operation and a lost packet costs one sync search and, if it was the
 *  dwell's last, nothing more; FIRSTRXTIMEOUT is 1.25 times %packetsPerHop
 *  periods, so an operation that hears nothing ends 1.25 dwells after its
 *  post and a receiver that has not found the transmitter cycles the
 *  channels at 1.25 dwells per channel against the transmitter's 1.0,
 *  overlaps it within a few dwells, and is aligned exactly by the first
 *  packet's hopSync. The rows are captured, and go stale, as on the TX
 *  stream; %rfFrequency is not used. NRXOK and NRXNOK restart at every
 *  operation and the handler accumulates them, so the target arithmetic
 *  never wraps; %stats.nRxOk and %stats.nRxNok report the totals and
 *  %stats.nHops the hops made. A table without rows, with fewer than two
 *  channels or a zero frequency in it, with %packetsPerHop or
 *  %packetPeriodTicks zero, with a dwell timeout under 128 us or over what
 *  16 bits hold, or with a stop time in %common.timing ends the command with
 *  RCL_CommandStatus_Error_Param.
 *
 *  A stop of a hopping command is never written into the running operation:
 *  with the synthesizer kept on, the PBE's end routine after a stop in sync
 *  search waits for a report the RFE only gives when told to turn the
 *  synthesizer off, and never returns (measured). The handler tells RCL to
 *  post the stop as an event and write nothing to the PBE, and answers it at
 *  the next operation end, which it brings about: it arms the packet count
 *  so that the next packet ends the operation, and if none comes the
 *  timeouts do, one period after the last packet or 1.25 dwells after the
 *  post. The operation in flight ends, the handler does not post again and
 *  the command ends with the stop's status, within a packet period under
 *  traffic (measured: 100 to 200 us). A hard stop is therefore no sooner
 *  than a graceful one on a hopping command. Only the stop API is answered
 *  this way: a stop time, the command's own or a scheduler's for a queued
 *  successor, reaches the PBE through the SysTimer compares and would be
 *  written into the running operation, so the command refuses stop times
 *  at setup and a command queued behind it must use
 *  RCL_ConflictPolicy_NeverInterrupt. The synthesizer is still on when the
 *  command ends: with
 *  %config.disableLRF set LRF_disable() takes it down with the front end,
 *  and with it cleared it is left running, as after an FS command, for a
 *  following command that programs no frequency.
 *
 *  @note CC27XX only. Other devices end the command with RCL_CommandStatus_Error_Param.
 */
struct RCL_CMD_GENERIC_RX_STREAM_t {
    RCL_Command             common;
    uint32_t                rfFrequency;            /*!< RF frequency in Hz. 0: do not program frequency */
    uint32_t                syncWord;               /*!< Sync word to match */
    uint16_t                packetLength;           /*!< Payload bytes per packet */
    uint16_t                entryBytes;             /*!< Written by the handler at setup: bytes of one entry in the RX FIFO, padded to a word */
    RCL_StatsGenericRxStream *stats;                /*!< Pointer to statistics structure. NULL: Do not store statistics */
    struct {
        uint8_t             fsOff:     1;           /*!< Without a hop table: the synthesizer is always off after the command. 0: keep refsys and the power constraints for a following command that programs its frequency. 1: release them */
        uint8_t             enableLRF: 1;           /*!< 1: call LRF_enable() at command start. 0: assume the LRF is already enabled by a previous command */
        uint8_t             disableLRF:1;           /*!< 1: call LRF_disable() at command end. 0: leave the LRF enabled for a following command; not supported after a stop, see above */
        uint8_t             reserved:  5;           /*!< Reserved, set to 0 */
    } config;
    const uint32_t         *hopFrequencies;         /*!< Frequency hop table in Hz, taken in order. NULL: no hopping */
    uint32_t               *hopRows;                /*!< With a table: %numHops * %RCL_GENERIC_HOP_ROW_WORDS words the handler fills at setup and reads at every hop; must live for the life of the command */
    uint8_t                 numHops;                /*!< Channels in the table, 2 or more */
    uint8_t                 packetsPerHop;          /*!< Packets the transmitter sends on a channel before it hops, 1 or more */
    uint16_t                packetPeriodTicks;      /*!< With a table: the transmitter's packet period in 0.25 us ticks, from which the sync-search timeouts are set */
};

#define RCL_CmdGenericRxStream_Default()                                        \
{                                                                               \
    .common = RCL_Command_Default(RCL_CMDID_GENERIC_RX_STREAM,                  \
                                  RCL_Handler_Generic_RxStream),                \
    .rfFrequency          = 2440000000U,                                        \
    .syncWord             = 0x930B51DEU,                                        \
    .packetLength         = 24U,                                                \
    .entryBytes           = 0U,                                                 \
    .stats                = NULL,                                               \
    .config = { .fsOff = 1, .enableLRF = 1, .disableLRF = 1, .reserved = 0 },   \
    .hopFrequencies       = NULL,                                               \
    .hopRows              = NULL,                                               \
    .numHops              = 0U,                                                 \
    .packetsPerHop        = 0U,                                                 \
    .packetPeriodTicks    = 0U,                                                 \
}
#define RCL_CmdGenericRxStream_DefaultRuntime() \
    (RCL_CmdGenericRxStream) RCL_CmdGenericRxStream_Default()

/**
 *  @brief Align a hopping RX stream with the transmitter's packet counter
 *
 *  For a command with a hop table, to be called from the application's
 *  per-packet callback for every packet accepted, with the packet counter
 *  the transmitter put in it. The transmitter counts from 0 at the first
 *  packet it sends on the table's first channel, so counter / packetsPerHop
 *  is the dwell and counter mod packetsPerHop the position in it; only the
 *  position is used, the dwell being implied by the channel the packet was
 *  heard on. The call arms NRXTARGET so that the running operation ends
 *  with the dwell's last packet: the packets counted in this operation so
 *  far, CRC failures included, plus those still to come. The PBE ends the
 *  operation at a packet's end when NRXOK + NRXNOK has reached NRXTARGET
 *  (compared as not less than, PBE bank 0 at 0x27b). That the count
 *  includes the packet in hand by the time the callback runs rests on
 *  measurement, not on a reading of the image's order of count and commit:
 *  the target so armed landed the operation on the dwell's last packet in
 *  59 999 of 59 999 hops. For the last packet itself it writes
 *  nothing: the operation is ending on the count an earlier call armed, or,
 *  when no earlier packet of the dwell was heard, on the timeout one period
 *  later. The receiver's channel is the one the packet was heard on, so a
 *  receiver out of phase, because it started first or lost a whole dwell,
 *  is aligned by the first packet it hears.
 *
 *  It runs in the caller's context, a few loads and stores and no RCL call,
 *  and must run before the next packet of the dwell ends, which is one
 *  period at most; a call later than that arms a target for the wrong
 *  operation, which costs one dwell's alignment and no more. It does nothing
 *  when the command is not active or has no hop table. The counter must not
 *  wrap during the command: 2^32 is not a multiple of packetsPerHop times
 *  numHops in general.
 *
 *  @param cmd            The command
 *  @param packetCounter  The transmitter's counter of the packet just received
 */
void RCL_CmdGenericRxStream_hopSync(RCL_CmdGenericRxStream *cmd, uint32_t packetCounter);

struct RCL_STATS_GENERIC_TX_STREAM_t {
    struct
    {
        uint8_t accumulate : 1;  /*!< 0: Reset counter to 0 at start of command. 1: Add to incoming value of counter. */
        uint8_t reserved : 7;    /*!< Reserved, set to 0 */
    } config;                    /*!< Configuration provided to RCL */
    uint16_t nTx;                /*!< Number of packets transmitted */
    uint16_t nHops;              /*!< Number of hops made, with a hop table, the missed ones included */
    uint16_t nHopsMissed;        /*!< Of those, hops whose row was not written because the RFE was busy with the next operation */
};

#define RCL_StatsGenericTxStream_Default() \
{                                          \
    .config = { 0 },                       \
    .nTx = 0,                              \
    .nHops = 0,                            \
    .nHopsMissed = 0,                      \
}
#define RCL_StatsGenericTxStream_DefaultRuntime() (RCL_StatsGenericTxStream) RCL_StatsGenericTxStream_Default()

struct RCL_STATS_GENERIC_RX_STREAM_t {
    struct
    {
        uint8_t accumulate : 1;  /*!< 0: Reset counters to 0 at start of command. 1: Add to incoming value of counters. */
        uint8_t reserved : 7;    /*!< Reserved, set to 0 */
    } config;                    /*!< Configuration provided to RCL */
    uint16_t nRxOk;              /*!< Number of packets received with correct CRC */
    uint16_t nRxNok;             /*!< Number of packets received with CRC error */
    uint16_t nHops;              /*!< Number of hops made, with a hop table; on the receive path a hop cannot miss its row */
};

#define RCL_StatsGenericRxStream_Default() \
{                                          \
    .config = { 0 },                       \
    .nRxOk = 0,                            \
    .nRxNok = 0,                           \
    .nHops = 0,                            \
}
#define RCL_StatsGenericRxStream_DefaultRuntime() (RCL_StatsGenericRxStream) RCL_StatsGenericRxStream_Default()


#endif /* ti_drivers_rcl_commands_generic__include */
