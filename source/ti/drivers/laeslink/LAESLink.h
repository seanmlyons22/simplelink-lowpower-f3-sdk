/*
 * Copyright (c) 2026, Texas Instruments Incorporated
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

/*
 *  ======== LAESLink.h ========
 *  Zero-CPU AES-CCM datapath between SRAM, the LAES and the radio FIFO.
 *
 *  Transmit: a software request (from a relay channel, or from the CPU) starts
 *  a peripheral scatter-gather task list on uDMA channel 8. The list writes
 *  the CCM blocks into the LAES, waits for each AESDONE through the event
 *  fabric, moves the ciphertext from TXT straight into the radio TX FIFO,
 *  forms the MIC with the TXTX hardware XOR, posts the PBE operation and
 *  advances the packet counter with the LAES counter hardware. No CPU
 *  instruction runs per packet.
 *
 *  Receive: the same machinery decrypts and recomputes the MIC on channel 9.
 *  The last data task of the list raises a GPIO interrupt through GPIO.ISET;
 *  the one interrupt per packet does the constant-time compare, the replay
 *  check and the queue advance.
 *
 *  Packet on air, fixed length 24 bytes: hdr[4] = packet counter, big endian,
 *  which is the AAD | ct[16] | mic[4]. CCM with L = 2, M = 4 and the 13-byte
 *  nonce sid[3] | counter[4] | tail[6]. A FIFO entry is seven words: the
 *  length word, the header, four ciphertext words and the MIC.
 *
 *  Key: a plaintext 16-byte key is written to the LAES KEY registers with
 *  AESWriteKEY at open and overwritten with zeros at close. The caller owns
 *  its copy and clears it with CryptoUtils_memset once open has returned.
 *
 *  The uDMA and the LAES lose their state in standby, so a link holds
 *  PowerLPF3_DISALLOW_STANDBY from open to close.
 */

#ifndef ti_drivers_laeslink_LAESLink__include
#define ti_drivers_laeslink_LAESLink__include

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Packet geometry. The datapath moves exactly one payload block, the B1
 * image assumes a 4-byte header and the nonce length follows from L = 2.
 */
#define LAESLINK_AAD_LEN        (4U)
#define LAESLINK_PAYLOAD_LEN    (16U)
#define LAESLINK_MIC_LEN        (4U)
#define LAESLINK_NONCE_LEN      (13U)
#define LAESLINK_KEY_LEN        (16U)
#define LAESLINK_FIFO_WORDS     (7U)
#define LAESLINK_SLOTS          (2U)

_Static_assert(LAESLINK_AAD_LEN == 4U, "the B1 layout assumes a 4-byte header");
_Static_assert(LAESLINK_PAYLOAD_LEN == 16U, "the datapath moves exactly one payload block");
_Static_assert(LAESLINK_MIC_LEN == 4U, "the MIC is one word of TXT");
_Static_assert(LAESLINK_NONCE_LEN == 15U - 2U, "L = 2");
_Static_assert(LAESLINK_FIFO_WORDS == 1U + 1U + LAESLINK_PAYLOAD_LEN / 4U + 1U, "length, header, ciphertext, MIC");

/* Absolute transmit counter limit, not a packet count relative to
 * initialCounter. The DMA does not enforce it; the application checks the
 * watchdog and closes or rekeys before the 32-bit counter could carry into
 * the session identifier.
 */
#define LAESLINK_COUNTER_LIMIT  (1UL << 31)

/* Status codes */
#define LAESLINK_STATUS_SUCCESS         (0)
#define LAESLINK_STATUS_ERROR_CONFIG    (-1)   /* Counter at or past the limit, or a DIO past 31 */
#define LAESLINK_STATUS_ERROR_KEY       (-2)   /* STA.KEYSTATE did not become valid */

/* Faults reported by the transmit watchdog */
typedef enum
{
    LAESLink_Fault_None = 0,
    LAESLink_Fault_Stalled,           /* No packet completed between two watchdog calls */
    LAESLink_Fault_DmaError,          /* The uDMA reported a bus error */
    LAESLink_Fault_KeyInvalid,        /* The key registers are no longer valid */
    LAESLink_Fault_CounterExhausted,  /* The counter reached the session limit */
} LAESLink_Fault;

/* Session parameters */
typedef struct
{
    uint8_t  sid[3];           /* Nonce octets 0..2: session identifier */
    uint8_t  tail[6];          /* Nonce octets 7..12: direction and reserved bytes */
    uint32_t initialCounter;   /* First transmit counter. Below LAESLINK_COUNTER_LIMIT; never repeats under one key */
    uint32_t fifoLengthWord;   /* Word written first into every FIFO entry: length and pad, PBE defined */
    uint8_t  debugDio;         /* Stage pin the lists toggle once per packet through DOUTTGL31_0 */
    uint8_t  doorbellDio;      /* Receive: the pin whose interrupt the list raises through GPIO.ISET */
} LAESLink_Config;

/* Both pins are configured as outputs by the application (SysConfig); the
 * link only writes the GPIO toggle and interrupt-set registers from its
 * task lists.
 */

/* Where the transmit list writes the packet */
typedef enum
{
    LAESLink_TxSink_RadioFifo,  /* LRFDTXF.TXD, then OP_TX to LRFDPBE.API */
    LAESLink_TxSink_Memory,     /* The capture array, one entry per slot, for tests without a radio */
} LAESLink_TxSink;

/* How received packets reach the pipeline */
typedef enum
{
    LAESLink_RxSource_Memory,   /* The CPU places FIFO entries with LAESLink_rxInject */
    LAESLink_RxSource_Radio,    /* uDMA channel 2 drains the radio RX FIFO into the ring; a relay starts the list */
} LAESLink_RxSource;

/* Outcome of one received packet */
typedef enum
{
    LAESLink_RxStatus_Accepted,   /* MIC verified, counter fresh */
    LAESLink_RxStatus_MicFailed,  /* MIC did not verify. The payload was discarded. */
    LAESLink_RxStatus_Replay,     /* Counter not above the last accepted one */
} LAESLink_RxStatus;

/* Called from the doorbell interrupt for every packet. payload is the 16
 * decrypted bytes, valid for the duration of the call, and NULL unless the
 * packet was accepted.
 */
typedef void (*LAESLink_RxCallback)(LAESLink_RxStatus status, uint32_t counter, const uint8_t *payload);

/*
 *  Transmit
 */

/* Build the datapath, load the key and arm the channels. Nothing runs until
 * LAESLink_txStart.
 */
int_fast16_t LAESLink_txOpen(const LAESLink_Config *cfg, const uint8_t key[LAESLINK_KEY_LEN], LAESLink_TxSink sink);

/* Enable channels 8 and 9. With the radio sink, the radio command must be
 * running before this is called. The sample source (the SPI channel) is
 * armed by the application after this returns, so that its first completion
 * finds the relay ready.
 */
void LAESLink_txStart(void);

/* Wait for a packet in flight to finish, then disable channels 8 and 9. The
 * application stops the sample source first.
 */
void LAESLink_txStop(void);

/* Issue one packet request from software: SOFTREQ[8]. For tests, and for a
 * sender with no relay.
 */
void LAESLink_txKick(void);

/* True while channel 8 is inside a packet list. The refresh task at the end
 * of every packet restores the primary to its full count, so a fresh primary
 * means idle. This is an idle check, not a completion signal: it also reads
 * false in the window between a request and the controller's first copy
 * step. Wait for LAESLink_txPacketsSent to advance, then for this to return
 * false, before reusing a list.
 */
bool LAESLink_txPacketInFlight(void);

/* Write the plaintext for ring slot slot. Packet n uses slot n % SLOTS;
 * write slot n before request n.
 */
void LAESLink_txWritePayload(uint32_t slot, const uint8_t payload[LAESLINK_PAYLOAD_LEN]);

/* Packets fully written to the sink since the link opened, not packets sent
 * over the air. The list can still be refreshing its images; see
 * LAESLink_txPacketInFlight.
 */
uint32_t LAESLink_txPacketsSent(void);

/* Counter the next packet will carry. Advances after S0, before the packet is
 * complete.
 */
uint32_t LAESLink_txNextCounter(void);

/* The captured FIFO entry for slot (memory sink only) */
void LAESLink_txCapture(uint32_t slot, uint32_t out[LAESLINK_FIFO_WORDS]);

/* Health check. Call every N packets, never per packet. With expectProgress
 * set, a completed count equal to the previous call's is a stall.
 */
LAESLink_Fault LAESLink_txWatchdog(bool expectProgress);

/* Tear down in the order the LAES requires: channels off before abort. Zeroes
 * the key registers and the plaintext ring.
 */
void LAESLink_txClose(void);

/*
 *  Receive
 */

/* Build the datapath, load the key and enable the channels. With the radio
 * source, the radio command must already be running with its FIFO commit
 * trigger routed to channel 2.
 */
int_fast16_t LAESLink_rxOpen(const LAESLink_Config *cfg,
                             const uint8_t key[LAESLINK_KEY_LEN],
                             LAESLink_RxSource source,
                             LAESLink_RxCallback onPacket);

/* Feed one FIFO entry (length, header, C0..C3, MIC) and start the pipeline on
 * it. Memory source only. Returns false until the previous packet's doorbell
 * has been handled: a second request during AES would advance the active
 * list early.
 */
bool LAESLink_rxInject(const uint32_t entry[LAESLINK_FIFO_WORDS]);

/* The body of the doorbell interrupt: register it as the GPIO callback of
 * cfg->doorbellDio. Verifies the finished packet, reports it through the
 * callback, zeroes the plaintext slot and advances the ring.
 */
void LAESLink_rxDoorbell(void);

/* Channels off before abort. Zeroes the key registers and the output records. */
void LAESLink_rxClose(void);

#ifdef __cplusplus
}
#endif

#endif /* ti_drivers_laeslink_LAESLink__include */
