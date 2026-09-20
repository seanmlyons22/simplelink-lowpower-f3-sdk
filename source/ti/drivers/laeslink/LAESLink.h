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

/*!****************************************************************************
 *  @file       LAESLink.h
 *  @brief      Zero-CPU AES-CCM datapath between SRAM, the LAES and the radio
 *              FIFO
 *
 *  @anchor ti_drivers_laeslink_Overview
 *  # Overview #
 *
 *  Transmit: a software request (from a relay channel, or from the CPU)
 *  starts a peripheral scatter-gather task list on uDMA channel 8. The list
 *  writes the CCM blocks into the LAES, waits for each AESDONE through the
 *  event fabric, moves the ciphertext from TXT straight into the radio TX
 *  FIFO, forms the MIC with the TXTX hardware XOR, posts the PBE operation
 *  and advances the packet counter with the LAES counter hardware. No CPU
 *  instruction runs per packet.
 *
 *  Receive: the same machinery decrypts and recomputes the MIC on channel 9.
 *  The last data task of the list raises a GPIO interrupt through GPIO.ISET;
 *  the one interrupt per packet does the constant-time compare, the replay
 *  check and the queue advance.
 *
 *  A device is a transmitter or a receiver, never both at once: both
 *  sessions abort the LAES at open and close and both own uDMA channel 9.
 *
 *  # Packet #
 *
 *  On air, fixed length 24 bytes: hdr[4] = packet counter, big endian, which
 *  is the AAD | ct[16] | mic[4]. CCM with L = 2, M = 4 and the 13-byte nonce
 *  sid[3] | counter[4] | tail[6]. A FIFO entry is seven words: the length
 *  word, the header, four ciphertext words and the MIC.
 *
 *  # Key #
 *
 *  A plaintext 16-byte key is written to the LAES KEY registers with
 *  AESWriteKEY at open and overwritten with zeros at close. The caller owns
 *  its copy and clears it with CryptoUtils_memset once open has returned.
 *
 *  # Power #
 *
 *  The uDMA and the LAES lose their state in standby, so a link holds
 *  PowerLPF3_DISALLOW_STANDBY from open to close.
 *
 *  # Pins #
 *
 *  Both pins in the configuration are set up as outputs by the application
 *  (SysConfig); the link only writes the GPIO toggle and interrupt-set
 *  registers from its task lists.
 ******************************************************************************
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

/*! @brief  uDMA channel the transmit sample source runs on
 *
 *  A fabric channel, so that the peripheral's event reaches it edge
 *  detected; the link allocates its control table entry.
 */
#define LAESLINK_TX_SAMPLE_CH   (10U)

_Static_assert(LAESLINK_AAD_LEN == 4U, "the B1 layout assumes a 4-byte header");
_Static_assert(LAESLINK_PAYLOAD_LEN == 16U, "the datapath moves exactly one payload block");
_Static_assert(LAESLINK_MIC_LEN == 4U, "the MIC is one word of TXT");
_Static_assert(LAESLINK_NONCE_LEN == 15U - 2U, "L = 2");
_Static_assert(LAESLINK_FIFO_WORDS == 1U + 1U + LAESLINK_PAYLOAD_LEN / 4U + 1U, "length, header, ciphertext, MIC");

/*!
 *  @brief  Absolute transmit counter limit
 *
 *  Not a packet count relative to the initial counter. The DMA does not
 *  enforce it; the application checks the watchdog and closes or rekeys
 *  before the 32-bit counter could carry into the session identifier.
 */
#define LAESLINK_COUNTER_LIMIT  (1UL << 31)

/*! @brief  The call succeeded */
#define LAESLINK_STATUS_SUCCESS         (0)
/*! @brief  Counter at or past the limit, a DIO past 31, or a NULL receive callback */
#define LAESLINK_STATUS_ERROR_CONFIG    (-1)
/*! @brief  STA.KEYSTATE did not become valid after the key was written */
#define LAESLINK_STATUS_ERROR_KEY       (-2)

/*!
 *  @brief  Faults reported by the transmit watchdog
 */
typedef enum
{
    LAESLink_Fault_None = 0,
    LAESLink_Fault_Stalled,           /*!< No packet completed between two watchdog calls */
    LAESLink_Fault_DmaError,          /*!< The uDMA reported a bus error */
    LAESLink_Fault_KeyInvalid,        /*!< The key registers are no longer valid */
    LAESLink_Fault_CounterExhausted,  /*!< The counter reached the session limit */
} LAESLink_Fault;

/*!
 *  @brief  Session parameters
 */
typedef struct
{
    uint8_t  sid[3];           /*!< Nonce octets 0..2: session identifier */
    uint8_t  tail[6];          /*!< Nonce octets 7..12: direction and reserved bytes */
    uint32_t initialCounter;   /*!< First transmit counter. Below #LAESLINK_COUNTER_LIMIT; never repeats under one key */
    uint32_t fifoLengthWord;   /*!< Word written first into every FIFO entry: length and pad, PBE defined */
    uint8_t  debugDio;         /*!< Stage pin the lists toggle once per packet through DOUTTGL31_0 */
    uint8_t  doorbellDio;      /*!< Receive: the pin whose interrupt the list raises through GPIO.ISET */
} LAESLink_Config;

/*!
 *  @brief  Where the transmit list writes the packet
 */
typedef enum
{
    LAESLink_TxSink_RadioFifo,  /*!< LRFDTXF.TXD, then OP_TX to LRFDPBE.API */
    LAESLink_TxSink_Memory,     /*!< The capture array, one entry per slot, for tests without a radio */
} LAESLink_TxSink;

/*!
 *  @brief  How received packets reach the pipeline
 */
typedef enum
{
    LAESLink_RxSource_Memory,   /*!< The CPU places FIFO entries with LAESLink_rxInject() */
    LAESLink_RxSource_Radio,    /*!< uDMA channel 2 drains the radio RX FIFO into the ring; a relay starts the list */
} LAESLink_RxSource;

/*!
 *  @brief  Outcome of one received packet
 */
typedef enum
{
    LAESLink_RxStatus_Accepted,   /*!< MIC verified, counter fresh */
    LAESLink_RxStatus_MicFailed,  /*!< MIC did not verify. The payload was discarded. */
    LAESLink_RxStatus_Replay,     /*!< Counter not above the last accepted one */
} LAESLink_RxStatus;

/*!
 *  @brief  Called from the doorbell interrupt for every packet
 *
 *  @param  status   Outcome of the packet
 *  @param  counter  Packet counter from the header
 *  @param  payload  The 16 decrypted bytes, valid for the duration of the
 *                   call, and NULL unless the packet was accepted
 */
typedef void (*LAESLink_RxCallback)(LAESLink_RxStatus status, uint32_t counter, const uint8_t *payload);

/*!
 *  @brief  Build the transmit datapath, load the key and arm the channels
 *
 *  Nothing runs until LAESLink_txStart().
 *
 *  @param  cfg   Session parameters
 *  @param  key   Plaintext key; clear the caller's copy after this returns
 *  @param  sink  The radio FIFO or the capture array
 *
 *  @retval #LAESLINK_STATUS_SUCCESS
 *  @retval #LAESLINK_STATUS_ERROR_CONFIG  initialCounter at or past the limit, or debugDio past 31
 *  @retval #LAESLINK_STATUS_ERROR_KEY     the key did not load
 *
 *  @pre    No receive session is open on this device
 */
int_fast16_t LAESLink_txOpen(const LAESLink_Config *cfg, const uint8_t key[LAESLINK_KEY_LEN], LAESLink_TxSink sink);

/*!
 *  @brief  Attach the sample channel that fills the plaintext ring
 *
 *  The sample source is uDMA channel #LAESLINK_TX_SAMPLE_CH, which moves one
 *  payload, #LAESLINK_PAYLOAD_LEN bytes as 16-bit items in one arbitration,
 *  from a fixed data register into the ring slot the next packet encrypts.
 *  Its completion is what starts a packet, so the sample is the clock of the
 *  link and there is no timer to drift against it.
 *
 *  The relay clears the channel's done flag, re-arms it into the next slot
 *  and enables it again before it starts the packet list on this one, so
 *  the list owns the ring index: a completion that is missed costs a sample
 *  and can never leave the two channels encrypting and filling the same
 *  slot.
 *
 *  The channel is a fabric channel, and the application subscribes it to the
 *  peripheral's event with EVTSVTConfigureDma(). That event must be one the
 *  peripheral raises once per payload, which for a FIFO means its level
 *  interrupt with the trigger level set to a whole payload: the fabric
 *  edge-detects, so one event is one arbitration, where a peripheral
 *  request line is a level the arbitration has to take down and a level
 *  that survives it parks the controller. Measured on a CC2755P20: SPI0's
 *  own receive request line moves one arbitration and then waits for ever,
 *  while SPI0_COMB published to this channel runs indefinitely.
 *
 *  Such an event is latched, so the relay clears it once per payload with
 *  a write of %ackMask to %ackRegister, after the sample is out of the
 *  peripheral and the channel is armed again.
 *
 *  This programs the channel's control table entry, its DMA.DONEMASK bit
 *  (which is what publishes its completion to the fabric) and the relay.
 *  The peripheral stays the application's: it routes the event, enables the
 *  channel after LAESLink_txStart(), and disables it before
 *  LAESLink_txStop().
 *
 *  @param  dataRegister  The peripheral's receive data register
 *  @param  ackRegister   Register that clears the peripheral's event
 *  @param  ackMask       Value written to it, once per payload
 *
 *  @pre    LAESLink_txOpen() has returned and LAESLink_txStart() has not
 *          been called
 */
void LAESLink_txAttachSampleSource(uint32_t dataRegister, uint32_t ackRegister, uint32_t ackMask);

/*!
 *  @brief  Enable channels 8 (the packet list) and 9 (the relay)
 *
 *  The relay subscribes to DMA_DONE_COMB by design: it is the sample
 *  channel's completion that starts a packet. The application enables that
 *  channel after this returns, so that its first completion finds the relay
 *  ready; see LAESLink_txAttachSampleSource(). Without a sample source the
 *  packets come from LAESLink_txKick().
 *
 *  @pre    With the radio sink, the radio command is running
 */
void LAESLink_txStart(void);

/*!
 *  @brief  Wait for a packet in flight to finish, then disable channels 8 and 9
 *
 *  @pre    The application has stopped the sample source
 */
void LAESLink_txStop(void);

/*!
 *  @brief  Issue one packet request from software: SOFTREQ[8]
 *
 *  For tests, and for a sender with no relay.
 */
void LAESLink_txKick(void);

/*!
 *  @brief  True while channel 8 is inside a packet list
 *
 *  The refresh task at the end of every packet restores the primary to its
 *  full count, so a fresh primary means idle. This is an idle check, not a
 *  completion signal: it also reads false in the window between a request
 *  and the controller's first copy step. Wait for LAESLink_txPacketsSent()
 *  to advance, then for this to return false, before reusing a list.
 *
 *  @retval true   a list is running
 *  @retval false  channel 8 is idle
 */
bool LAESLink_txPacketInFlight(void);

/*!
 *  @brief  Write the plaintext for a ring slot
 *
 *  Packet n uses slot n % #LAESLINK_SLOTS; write slot n before request n.
 *
 *  @param  slot     Ring slot
 *  @param  payload  The 16 plaintext bytes
 */
void LAESLink_txWritePayload(uint32_t slot, const uint8_t payload[LAESLINK_PAYLOAD_LEN]);

/*!
 *  @brief  Packets fully written to the sink since the link opened
 *
 *  Not packets sent over the air. The list can still be refreshing its
 *  images; see LAESLink_txPacketInFlight().
 *
 *  @return Completed packet count
 */
uint32_t LAESLink_txPacketsSent(void);

/*!
 *  @brief  Counter the next packet will carry
 *
 *  Advances after S0, before the packet is complete.
 *
 *  @return The counter of record
 */
uint32_t LAESLink_txNextCounter(void);

/*!
 *  @brief  The captured FIFO entry for a slot
 *
 *  @param  slot  Ring slot
 *  @param  out   The seven entry words
 *
 *  @pre    The link was opened with #LAESLink_TxSink_Memory
 */
void LAESLink_txCapture(uint32_t slot, uint32_t out[LAESLINK_FIFO_WORDS]);

/*!
 *  @brief  Health check
 *
 *  Call every N packets, never per packet.
 *
 *  @param  expectProgress  A completed count equal to the previous call's is a stall
 *
 *  @return The first fault found, or #LAESLink_Fault_None
 */
LAESLink_Fault LAESLink_txWatchdog(bool expectProgress);

/*!
 *  @brief  Tear the transmit session down
 *
 *  Channels off before the LAES abort, in the order the LAES requires.
 *  Zeroes the key registers and the plaintext ring.
 */
void LAESLink_txClose(void);

/*!
 *  @brief  Build the receive datapath, load the key and enable the channels
 *
 *  @param  cfg       Session parameters
 *  @param  key       Plaintext key; clear the caller's copy after this returns
 *  @param  source    Injection, or the radio drain
 *  @param  onPacket  Called from the doorbell interrupt for every packet
 *
 *  @retval #LAESLINK_STATUS_SUCCESS
 *  @retval #LAESLINK_STATUS_ERROR_CONFIG  a DIO past 31, or onPacket NULL
 *  @retval #LAESLINK_STATUS_ERROR_KEY     the key did not load
 *
 *  @pre    No transmit session is open on this device
 *  @pre    With the radio source, the radio command is running with its FIFO
 *          commit trigger routed to channel 2
 *  @pre    cfg->doorbellDio has LAESLink_rxDoorbell() registered as its GPIO
 *          callback with its interrupt enabled and no edge detection
 */
int_fast16_t LAESLink_rxOpen(const LAESLink_Config *cfg,
                             const uint8_t key[LAESLINK_KEY_LEN],
                             LAESLink_RxSource source,
                             LAESLink_RxCallback onPacket);

/*!
 *  @brief  Feed one FIFO entry and start the pipeline on it
 *
 *  Memory source only. Refused until the previous packet's doorbell has
 *  returned: a second request during AES would advance the active list
 *  early. The pending flag is cleared after the callback returns, so an
 *  injection from inside the callback is refused as well.
 *
 *  @param  entry  Length word, header, C0..C3, MIC
 *
 *  @retval true   the entry was taken
 *  @retval false  refused
 */
bool LAESLink_rxInject(const uint32_t entry[LAESLINK_FIFO_WORDS]);

/*!
 *  @brief  The body of the doorbell interrupt
 *
 *  Register it as the GPIO callback of cfg->doorbellDio. Verifies the
 *  finished packet with a constant-time compare and the replay check,
 *  reports it through the callback, then zeroes the plaintext slot and
 *  advances the ring.
 */
void LAESLink_rxDoorbell(void);

/*!
 *  @brief  Tear the receive session down
 *
 *  Channels off before the LAES abort. Zeroes the key registers and the
 *  output records.
 */
void LAESLink_rxClose(void);

#ifdef __cplusplus
}
#endif

#endif /* ti_drivers_laeslink_LAESLink__include */
