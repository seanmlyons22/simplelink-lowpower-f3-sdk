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
 *  ======== LAESLink_lists.h ========
 *  Private to the link and its test harness: DMA-visible state, the task
 *  list builders and the helpers LAESLink.c shares between the transmit and
 *  receive sessions.
 *
 *  Every word the hardware reads or writes is volatile, so the compiler never
 *  caches it and the layout is stable. The CPU writes the lists before a
 *  channel is enabled and the hardware only reads them afterwards; the images
 *  and records are written by the hardware and read by the CPU.
 */

#ifndef ti_drivers_laeslink_LAESLink_lists__include
#define ti_drivers_laeslink_LAESLink_lists__include

#include <stdbool.h>
#include <stdint.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_aes.h)
#include DeviceFamily_constructPath(driverlib/udma.h)

#include <ti/drivers/laeslink/LAESLink.h>
#include <ti/drivers/laeslink/LAESLink_ccm.h>
#include <ti/drivers/laeslink/LAESLink_udma.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity of a transmit and of a receive task list, and the length of a
 * relay list. The relay primary is refreshed every packet, so its count only
 * needs to exceed one: a primary that ran out would disable the channel.
 *
 * The transmit relay is one group per ring slot, because re-arming the
 * sample channel names the slot it is re-armed into.
 */
#define LAESLINK_TX_LIST_CAP      (32U)
#define LAESLINK_RX_LIST_CAP      (32U)
#define LAESLINK_RELAY_TASKS      (8U)
#define LAESLINK_TX_RELAY_TASKS   (5U)

_Static_assert((LAESLINK_RELAY_TASKS % 2U) == 0U, "the receive relay is built in toggle/kick pairs");
_Static_assert(LAESLINK_TX_LIST_CAP * 4U <= LAESLINK_MAX_ITEMS, "a primary must cover the whole list");
_Static_assert(LAESLINK_RX_LIST_CAP * 4U <= LAESLINK_MAX_ITEMS, "a primary must cover the whole list");

/* uDMA channel masks */
#define LAESLINK_CH2    (UDMA_CHANNEL_2_M)
#define LAESLINK_CH8    (UDMA_CHANNEL_8_M)
#define LAESLINK_CH9    (UDMA_CHANNEL_9_M)
#define LAESLINK_CH10   (UDMA_CHANNEL_10_M)

/* AUTOCFG for the S0 operation on transmit: source BUF, single trigger on
 * BUF3, CTR64 left-aligned big-endian increment (advances the packet counter,
 * block word 1), bus halt as a safety net, RIS.AESDONE auto-clear.
 */
#define LAESLINK_CFG_S0_TX \
    (AES_AUTOCFG_AESSRC_BUF | AES_AUTOCFG_TRGAES_WRBUF3S | AES_AUTOCFG_CTRSIZE_CTR64 | \
     AES_AUTOCFG_CTRENDN_BIGENDIAN | AES_AUTOCFG_BUSHALT_EN | AES_AUTOCFG_CLRAESDN_EN)

/* AUTOCFG for S1 (both directions) and S0 on receive: as above without the
 * increment, and with the free-running BUF3 trigger instead of the one-shot,
 * which is legal because the counter is disabled here. Every block write under
 * this configuration starts an operation with no re-arm, so it also covers
 * the first CBC-MAC block: AESSRC = BUF computes AES(B0), which is X1 because
 * the CBC-MAC IV is zero. That is why nothing clears TXT.
 */
#define LAESLINK_CFG_S1 \
    (AES_AUTOCFG_AESSRC_BUF | AES_AUTOCFG_TRGAES_WRBUF3 | AES_AUTOCFG_BUSHALT_EN | AES_AUTOCFG_CLRAESDN_EN)

/* AUTOCFG for the CBC-MAC phase: TXT = AES(TXT ^ BUF) on every BUF3 write */
#define LAESLINK_CFG_MAC \
    (AES_AUTOCFG_AESSRC_TXTXBUF | AES_AUTOCFG_TRGAES_WRBUF3 | AES_AUTOCFG_BUSHALT_EN | AES_AUTOCFG_CLRAESDN_EN)

_Static_assert(LAESLINK_CFG_S0_TX == 0x03220028U, "CFG_S0_TX");
_Static_assert(LAESLINK_CFG_S1 == 0x03000024U, "CFG_S1");
_Static_assert(LAESLINK_CFG_MAC == 0x03000034U, "CFG_MAC");

/* Indices into the transmit constant block */
#define LAESLINK_TXC_CFG_S0     (0U)  /* AUTOCFG for S0 */
#define LAESLINK_TXC_CFG_S1     (1U)  /* AUTOCFG for S1 */
#define LAESLINK_TXC_CFG_MAC    (2U)  /* AUTOCFG for the CBC-MAC */
#define LAESLINK_TXC_KICK8      (3U)  /* Software request bit for channel 8 */
#define LAESLINK_TXC_LEN        (4U)  /* FIFO entry length word */
#define LAESLINK_TXC_GPIO       (5U)  /* Stage pin toggle mask */
#define LAESLINK_TXC_API        (6U)  /* OP_TX, written to LRFDPBE.API */
#define LAESLINK_TXC_BIT_SAMPLE (7U)  /* Sample channel bit, for REQDONE and SETCHANNELEN */
#define LAESLINK_TXC_ACK        (8U)  /* Written to the sample peripheral's event clear register */
#define LAESLINK_TXC_COUNT      (9U)

/* Indices into the receive constant block */
#define LAESLINK_RXC_CFG_S      (0U)  /* AUTOCFG for S0 and S1 (no increment) */
#define LAESLINK_RXC_CFG_MAC    (1U)  /* AUTOCFG for the CBC-MAC */
#define LAESLINK_RXC_KICK9      (2U)  /* Software request bit for channel 9 */
#define LAESLINK_RXC_BIT2       (3U)  /* Channel 2 bit */
#define LAESLINK_RXC_DOORBELL   (4U)  /* Doorbell pin mask, written to GPIO.ISET and DOUTTGL31_0 */
#define LAESLINK_RXC_GPIO       (5U)  /* Stage pin toggle mask, written by the relay */
#define LAESLINK_RXC_COUNT      (6U)

/* Layout of one receive output record (8 words) */
#define LAESLINK_RXO_PT         (0U)  /* Plaintext, four words */
#define LAESLINK_RXO_MIC_CALC   (4U)  /* MIC computed by the pipeline (word 0 of TXT after the XOR) */
#define LAESLINK_RXO_MIC_RX     (5U)  /* MIC received on air */
#define LAESLINK_RXO_HDR        (6U)  /* Header received on air */
#define LAESLINK_RXO_WORDS      (8U)

/* Layout of one received FIFO entry (7 words) */
#define LAESLINK_RXI_HDR        (1U)
#define LAESLINK_RXI_CT         (2U)
#define LAESLINK_RXI_MIC        (6U)

/* Transmit-side DMA memory */
typedef struct
{
    volatile LAESLink_Task lists[LAESLINK_SLOTS][LAESLINK_TX_LIST_CAP]; /* One task list per ring slot */
    volatile LAESLink_Task relay[LAESLINK_TX_RELAY_TASKS * LAESLINK_SLOTS]; /* Relay list for channel 9, one group per slot */
    volatile uint32_t prim[4U * LAESLINK_SLOTS];                        /* Channel 8 primary images, one per slot */
    volatile uint32_t relayPrim[4U * LAESLINK_SLOTS];                   /* Channel 9 primary images, one per slot */
    volatile uint32_t prim1[4U * LAESLINK_SLOTS];                       /* Sample channel primary images, one per slot */
    volatile uint32_t b0[4];                                            /* CCM B0 image */
    volatile uint32_t b1[4];                                            /* CCM B1 image */
    volatile uint32_t a0[4];                                            /* CCM A0 image */
    volatile uint32_t a1[4];                                            /* CCM A1 image */
    volatile uint32_t slots[4U * LAESLINK_SLOTS];                       /* Plaintext ring, 4 words per slot */
    volatile uint32_t s0[1];                                            /* Stashed S0 word 0 */
    volatile uint32_t consts[LAESLINK_TXC_COUNT];                       /* Constants */
    volatile uint32_t capture[LAESLINK_FIFO_WORDS * LAESLINK_SLOTS];    /* SRAM sink standing in for the FIFO */
} LAESLink_TxState;

/* Receive-side DMA memory */
typedef struct
{
    volatile LAESLink_Task lists[LAESLINK_SLOTS][LAESLINK_RX_LIST_CAP]; /* One task list per ring slot */
    volatile LAESLink_Task relay[LAESLINK_RELAY_TASKS];                 /* Relay list for channel 10 */
    volatile uint32_t prim[4U * LAESLINK_SLOTS];                        /* Channel 9 primary images, one per slot */
    volatile uint32_t relayPrim[4];                                     /* Channel 10 primary image */
    volatile uint32_t prim2[4U * LAESLINK_SLOTS];                       /* Channel 2 (radio RX FIFO) primary images */
    volatile uint32_t b0[4];                                            /* CCM B0 image */
    volatile uint32_t b1[4];                                            /* CCM B1 image */
    volatile uint32_t a0[4];                                            /* CCM A0 image */
    volatile uint32_t a1[4];                                            /* CCM A1 image */
    volatile uint32_t rx[LAESLINK_FIFO_WORDS * LAESLINK_SLOTS];         /* Received FIFO entries */
    volatile uint32_t out[LAESLINK_RXO_WORDS * LAESLINK_SLOTS];         /* Output records */
    volatile uint32_t s0[1];                                            /* Stashed S0 word 0 */
    volatile uint32_t consts[LAESLINK_RXC_COUNT];                       /* Constants */
} LAESLink_RxState;

/* What happens after a task completes */
typedef enum
{
    LAESLink_Flow_Continue, /* Auto-request, so the next task runs at once. Encoded as MEM_SG_ALT. */
    LAESLink_Flow_Wait,     /* Stop until the next request (an AESDONE edge, a relay, or the CPU). PER_SG_ALT. */
} LAESLink_Flow;

/* Appends tasks into a task list */
typedef struct
{
    volatile LAESLink_Task *list;
    uint32_t capacity;
    uint32_t n;
} LAESLink_ListBuilder;

void LAESLink_listBegin(LAESLink_ListBuilder *b, volatile LAESLink_Task *list, uint32_t capacity);
void LAESLink_listPush(LAESLink_ListBuilder *b, LAESLink_Task task, LAESLink_Flow flow);

/* Task helpers. All build the b111 cycle type; LAESLink_listPush sets the flow. */
LAESLink_Task LAESLink_word(uint32_t src, uint32_t dst);        /* One word, fixed addresses */
LAESLink_Task LAESLink_block(uint32_t src, uint32_t dst);       /* Four words, both addresses incrementing */
LAESLink_Task LAESLink_blockToFifo(uint32_t src, uint32_t dst); /* Four words from incrementing registers into a fixed port */
LAESLink_Task LAESLink_halfwords(uint32_t src, uint32_t dst);   /* Two halfwords, both addresses incrementing */

/* Address of a DMA-visible word */
static inline uint32_t LAESLink_addr(const volatile uint32_t *w)
{
    return (uint32_t)w;
}

/* Write a 16-byte block into four DMA-visible words */
static inline void LAESLink_setBlock(volatile uint32_t *dst, const uint8_t block[16])
{
    uint32_t w[4];

    LAESLink_blockToWords(block, w);
    for (uint32_t i = 0U; i < 4U; i++)
    {
        dst[i] = w[i];
    }
}

/* Build the transmit list for slot. Returns the number of entries. */
uint32_t LAESLink_buildTx(LAESLink_TxState *state, uint32_t slot, LAESLink_TxSink sink);

/* Build a relay list of n tasks. With gpioSrc zero every task kicks: it
 * writes kickSrc into DMA.SOFTREQ and waits. Otherwise the list is toggle/kick
 * pairs: the toggle writes gpioSrc into DOUTTGL31_0 and continues into the
 * kick.
 */
uint32_t LAESLink_buildRelay(volatile LAESLink_Task *list, uint32_t n, uint32_t kickSrc, uint32_t gpioSrc);

/* Build one slot's group of the transmit relay: acknowledge the sample
 * channel, re-arm it into the next slot, enable it and start the packet.
 * Returns the number of entries.
 */
uint32_t LAESLink_buildTxRelay(LAESLink_TxState *state, uint32_t slot, uint32_t ackRegister);

/* Build the receive list for slot. With radio set the list acknowledges and
 * re-arms channel 2 and refreshes the channel 10 relay. Returns the number of
 * entries.
 */
uint32_t LAESLink_buildRx(LAESLink_RxState *state, uint32_t slot, bool radio);

/*
 *  Shared by the sessions (LAESLink.c)
 */

/* Control table entries. The link allocates 8, 9 and 10 with their
 * alternates; any other channel resolves to its slot in the SysConfig table,
 * which is where the RCL template allocates the radio drain channel.
 */
volatile uDMAControlTableEntry *LAESLink_primary(uint32_t channel);
volatile uDMAControlTableEntry *LAESLink_alternate(uint32_t channel);

/* Make list memory visible to the uDMA before a channel is enabled or requested */
void LAESLink_publish(void);

/* Power dependencies, the standby constraint and the uDMA driver */
void LAESLink_open(void);
void LAESLink_close(void);

/* Abort anything in flight, clear the flags, load the key, enable the AESDONE
 * contribution to AES_COMB and program the first AUTOCFG. Returns false if
 * STA.KEYSTATE is not valid afterwards.
 */
bool LAESLink_laesOpen(const uint8_t key[LAESLINK_KEY_LEN], uint32_t autocfg);

/* Mask the interrupt, abort, clear the flags and zero the key registers */
void LAESLink_laesClose(void);

/* True while the key registers hold a key the core wrote */
bool LAESLink_keyValid(void);

/* Disable channels, clear their attributes, their done-mask bits and their
 * request-done flags.
 */
void LAESLink_channelsReset(uint32_t mask);

/* Route channel done signals to DMA_DONE_COMB (set bits) */
void LAESLink_doneMaskSet(uint32_t mask);

/* Bus error flag, cleared when read */
bool LAESLink_takeDmaError(void);

/* Byte swap: the counter lives big endian in the images */
static inline uint32_t LAESLink_swap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00U) | ((v << 8) & 0x00FF0000U) | (v << 24);
}

#ifdef __cplusplus
}
#endif

#endif /* ti_drivers_laeslink_LAESLink_lists__include */
