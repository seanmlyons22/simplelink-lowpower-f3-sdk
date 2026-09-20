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
 *  ======== LAESLink_tx.c ========
 *  Transmit session: once started it runs without any CPU involvement per
 *  packet.
 */

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/pbe_generic_regdef_regs.h)
#include DeviceFamily_constructPath(driverlib/evtsvt.h)
#include DeviceFamily_constructPath(driverlib/udma.h)

#include <ti/drivers/laeslink/LAESLink.h>
#include <ti/drivers/laeslink/LAESLink_ccm.h>
#include <ti/drivers/laeslink/LAESLink_lists.h>

/* DMA-visible memory. The uDMA addresses it for the life of the link. */
static LAESLink_TxState txState __attribute__((aligned(16)));

/* Session state the CPU alone touches */
static struct
{
    LAESLink_Config cfg;
    uint32_t tasks;     /* Entries in each slot's list */
    uint32_t lastSeen;  /* Completed counter at the previous watchdog call */
} tx;

/* Bound on the wait for a packet in flight: a packet takes about 30 us, this
 * is well over a millisecond of register reads.
 */
#define STOP_POLL_LIMIT (20000U)

/*
 *  ======== setBlock ========
 */
static void setBlock(volatile uint32_t *dst, const uint8_t block[16])
{
    uint32_t w[4];

    LAESLink_blockToWords(block, w);
    for (uint32_t i = 0U; i < 4U; i++)
    {
        dst[i] = w[i];
    }
}

/*
 *  ======== setImage ========
 */
static void setImage(volatile uint32_t *dst, LAESLink_Task t)
{
    dst[0] = t.srcEnd;
    dst[1] = t.dstEnd;
    dst[2] = t.control;
    dst[3] = t.spare;
}

/*
 *  ======== imageTask ========
 */
static LAESLink_Task imageTask(const volatile uint32_t *src)
{
    LAESLink_Task t;

    t.srcEnd  = src[0];
    t.dstEnd  = src[1];
    t.control = src[2];
    t.spare   = src[3];
    return t;
}

/*
 *  ======== completedCounter ========
 *  A1 is updated only after the ciphertext and MIC reach the sink, so that
 *  DMA write doubles as the completion signal.
 */
static uint32_t completedCounter(void)
{
    return LAESLink_swap32(txState.a1[LAESLINK_CCM_COUNTER_WORD]);
}

/*
 *  ======== LAESLink_txOpen ========
 */
int_fast16_t LAESLink_txOpen(const LAESLink_Config *cfg, const uint8_t key[LAESLINK_KEY_LEN], LAESLink_TxSink sink)
{
    LAESLink_CcmBlocks blocks;
    volatile uint32_t *c = txState.consts;
    uint32_t n;

    if ((cfg->initialCounter >= LAESLINK_COUNTER_LIMIT) || (cfg->debugDio >= 32U))
    {
        return LAESLINK_STATUS_ERROR_CONFIG;
    }

    LAESLink_open();

    /* Key, events, and the first AUTOCFG so that the first BUF3 write of the
     * list triggers.
     */
    if (!LAESLink_laesOpen(key, LAESLINK_CFG_S0_TX))
    {
        LAESLink_close();
        return LAESLINK_STATUS_ERROR_KEY;
    }

    /* CCM images for the first packet */
    LAESLink_ccmBlocks(&blocks, cfg->sid, cfg->tail, cfg->initialCounter);
    setBlock(txState.b0, blocks.b0);
    setBlock(txState.b1, blocks.b1);
    setBlock(txState.a0, blocks.a0);
    setBlock(txState.a1, blocks.a1);
    txState.s0[0] = 0U;

    /* Constants read by the task lists */
    c[LAESLINK_TXC_CFG_S0]  = LAESLINK_CFG_S0_TX;
    c[LAESLINK_TXC_CFG_S1]  = LAESLINK_CFG_S1;
    c[LAESLINK_TXC_CFG_MAC] = LAESLINK_CFG_MAC;
    c[LAESLINK_TXC_KICK8]   = LAESLINK_CH8;
    c[LAESLINK_TXC_LEN]     = cfg->fifoLengthWord;
    c[LAESLINK_TXC_GPIO]    = 1UL << cfg->debugDio;
    c[LAESLINK_TXC_API]     = PBE_GENERIC_REGDEF_API_OP_TX;

    /* Lists, one per slot, plus the primary image that points at each */
    for (uint32_t slot = 0U; slot < LAESLINK_SLOTS; slot++)
    {
        n = LAESLink_buildTx(&txState, slot, sink);
        setImage(&txState.prim[4U * slot],
                 LAESLink_sgPrimary(LAESLink_taskSpareAddr(&txState.lists[slot][n - 1U]),
                                    LAESLink_entrySpareAddr(LAESLink_alternate(8U)), n, true));
        tx.tasks = n;
    }
    n = LAESLink_buildRelay(txState.relay, LAESLINK_RELAY_TASKS, LAESLink_addr(&c[LAESLINK_TXC_KICK8]), 0U);
    setImage(txState.relayPrim,
             LAESLink_sgPrimary(LAESLink_taskSpareAddr(&txState.relay[n - 1U]),
                                LAESLink_entrySpareAddr(LAESLink_alternate(9U)), n, true));

    /* Control table: slot 0 list on channel 8, relay on channel 9 */
    LAESLink_writeEntry(LAESLink_primary(8U), imageTask(txState.prim));
    LAESLink_clearEntry(LAESLink_alternate(8U));
    LAESLink_writeEntry(LAESLink_primary(9U), imageTask(txState.relayPrim));
    LAESLink_clearEntry(LAESLink_alternate(9U));

    /* Event routing: the AESDONE edge paces the list, and a completion
     * published on DMA_DONE_COMB (the sample channel's, once the application
     * arms one) drives the relay. Writing DMACHnSEL clears EDGDETDIS, so a
     * level event becomes one request per rising edge.
     */
    EVTSVTConfigureDma(EVTSVT_DMA_CH8, EVTSVT_PUB_AES_COMB);
    EVTSVTConfigureDma(EVTSVT_DMA_CH9, EVTSVT_PUB_DMA_DONE_COMB);
    LAESLink_channelsReset(LAESLINK_CH8 | LAESLINK_CH9);

    tx.cfg      = *cfg;
    tx.lastSeen = cfg->initialCounter;
    return LAESLINK_STATUS_SUCCESS;
}

/*
 *  ======== LAESLink_txStart ========
 */
void LAESLink_txStart(void)
{
    LAESLink_publish();
    uDMAEnableChannel(LAESLINK_CH8 | LAESLINK_CH9);
}

/*
 *  ======== LAESLink_txStop ========
 */
void LAESLink_txStop(void)
{
    for (uint32_t i = 0U; (i < STOP_POLL_LIMIT) && LAESLink_txPacketInFlight(); i++)
    {
    }
    uDMADisableChannel(LAESLINK_CH8 | LAESLINK_CH9);
}

/*
 *  ======== LAESLink_txKick ========
 */
void LAESLink_txKick(void)
{
    LAESLink_publish();
    uDMARequestChannel(LAESLINK_CH8);
}

/*
 *  ======== LAESLink_txPacketInFlight ========
 */
bool LAESLink_txPacketInFlight(void)
{
    return LAESLink_taskItems(LAESLink_primary(8U)->control) != 4U * tx.tasks;
}

/*
 *  ======== LAESLink_txWritePayload ========
 */
void LAESLink_txWritePayload(uint32_t slot, const uint8_t payload[LAESLINK_PAYLOAD_LEN])
{
    setBlock(&txState.slots[4U * (slot % LAESLINK_SLOTS)], payload);
}

/*
 *  ======== LAESLink_txPacketsSent ========
 */
uint32_t LAESLink_txPacketsSent(void)
{
    return completedCounter() - tx.cfg.initialCounter;
}

/*
 *  ======== LAESLink_txNextCounter ========
 */
uint32_t LAESLink_txNextCounter(void)
{
    return LAESLink_swap32(txState.a0[LAESLINK_CCM_COUNTER_WORD]);
}

/*
 *  ======== LAESLink_txCapture ========
 */
void LAESLink_txCapture(uint32_t slot, uint32_t out[LAESLINK_FIFO_WORDS])
{
    const volatile uint32_t *entry = &txState.capture[(slot % LAESLINK_SLOTS) * LAESLINK_FIFO_WORDS];

    for (uint32_t i = 0U; i < LAESLINK_FIFO_WORDS; i++)
    {
        out[i] = entry[i];
    }
}

/*
 *  ======== LAESLink_txWatchdog ========
 */
LAESLink_Fault LAESLink_txWatchdog(bool expectProgress)
{
    uint32_t completed;

    if (LAESLink_takeDmaError())
    {
        return LAESLink_Fault_DmaError;
    }
    if (!LAESLink_keyValid())
    {
        return LAESLink_Fault_KeyInvalid;
    }
    if (LAESLink_txNextCounter() >= LAESLINK_COUNTER_LIMIT)
    {
        return LAESLink_Fault_CounterExhausted;
    }
    completed = completedCounter();
    if (expectProgress && (completed == tx.lastSeen))
    {
        return LAESLink_Fault_Stalled;
    }
    tx.lastSeen = completed;
    return LAESLink_Fault_None;
}

/*
 *  ======== LAESLink_txClose ========
 */
void LAESLink_txClose(void)
{
    LAESLink_txStop();
    LAESLink_clearEntry(LAESLink_primary(8U));
    LAESLink_clearEntry(LAESLink_primary(9U));
    LAESLink_channelsReset(LAESLINK_CH8 | LAESLINK_CH9);
    /* Channels off before abort: the LAES requires that order */
    LAESLink_laesClose();
    EVTSVTConfigureDma(EVTSVT_DMA_CH8, EVTSVT_PUB_NONE);
    EVTSVTConfigureDma(EVTSVT_DMA_CH9, EVTSVT_PUB_NONE);
    /* Nothing secret remains in the images, but clear the plaintext ring */
    for (uint32_t i = 0U; i < 4U * LAESLINK_SLOTS; i++)
    {
        txState.slots[i] = 0U;
    }
    LAESLink_close();
}
