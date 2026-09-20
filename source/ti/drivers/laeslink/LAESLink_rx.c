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
 *  ======== LAESLink_rx.c ========
 *  Receive session: one interrupt per packet.
 */

#include <stddef.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_lrfdpbe.h)
#include DeviceFamily_constructPath(driverlib/evtsvt.h)
#include DeviceFamily_constructPath(driverlib/udma.h)

#include <ti/drivers/cryptoutils/utils/CryptoUtils.h>
#include <ti/drivers/laeslink/LAESLink.h>
#include <ti/drivers/laeslink/LAESLink_ccm.h>
#include <ti/drivers/laeslink/LAESLink_lists.h>

/* The radio drain moves one committed entry per request through the PBE's
 * halfword read port: 28 bytes is 14 halfwords, and ARB_16 covers the entry
 * in one arbitration, which a pulse trigger needs. The port is
 * LRFDPBE.RXFHRD and not the LRFDRXF alias region the Rust link read: the
 * alias region carries the RCL-367 defect (a pop landing in the cycle after
 * a FIFO command is lost), see the RX stream comment in RCL_Dma.c.
 */
#define DRAIN_HALFWORDS (LAESLINK_FIFO_WORDS * 2U)

/* DMA-visible memory. The uDMA addresses it for the life of the link. */
static LAESLink_RxState rxState __attribute__((aligned(16)));

/* Session state the CPU alone touches */
static struct
{
    LAESLink_Config cfg;
    LAESLink_RxSource source;
    LAESLink_RxCallback onPacket;
    uint32_t consumeSlot;  /* Next slot the doorbell consumes */
    uint32_t injectSlot;   /* Next slot an injection fills */
    volatile bool pending; /* Set by an injection, cleared by the doorbell interrupt */
    bool haveAccepted;
    uint32_t lastAccepted;
} rx;

/*
 *  ======== LAESLink_rxOpen ========
 */
int_fast16_t LAESLink_rxOpen(const LAESLink_Config *cfg,
                             const uint8_t key[LAESLINK_KEY_LEN],
                             LAESLink_RxSource source,
                             LAESLink_RxCallback onPacket)
{
    LAESLink_CcmBlocks blocks;
    volatile uint32_t *c = rxState.consts;
    bool radio    = (source == LAESLink_RxSource_Radio);
    uint32_t mask = LAESLINK_CH9 | LAESLINK_CH10 | (radio ? LAESLINK_CH2 : 0U);
    uint32_t enable;
    uint32_t n;

    if ((cfg->debugDio >= 32U) || (cfg->doorbellDio >= 32U) || (onPacket == NULL))
    {
        return LAESLINK_STATUS_ERROR_CONFIG;
    }

    LAESLink_open();

    if (!LAESLink_laesOpen(key, LAESLINK_CFG_S1))
    {
        LAESLink_close();
        return LAESLINK_STATUS_ERROR_KEY;
    }

    /* Images: the counter word is replaced from the header before every packet */
    LAESLink_ccmBlocks(&blocks, cfg->sid, cfg->tail, 0U);
    LAESLink_setBlock(rxState.b0, blocks.b0);
    LAESLink_setBlock(rxState.b1, blocks.b1);
    LAESLink_setBlock(rxState.a0, blocks.a0);
    LAESLink_setBlock(rxState.a1, blocks.a1);
    rxState.s0[0] = 0U;

    c[LAESLINK_RXC_CFG_S]    = LAESLINK_CFG_S1;
    c[LAESLINK_RXC_CFG_MAC]  = LAESLINK_CFG_MAC;
    c[LAESLINK_RXC_KICK9]    = LAESLINK_CH9;
    c[LAESLINK_RXC_BIT2]     = LAESLINK_CH2;
    c[LAESLINK_RXC_DOORBELL] = 1UL << cfg->doorbellDio;
    c[LAESLINK_RXC_GPIO]     = 1UL << cfg->debugDio;

    /* Channel 2 images: one basic drain of a whole entry per slot */
    for (uint32_t slot = 0U; slot < LAESLINK_SLOTS; slot++)
    {
        LAESLink_setImage(&rxState.prim2[4U * slot],
                 LAESLink_transfer(LRFDPBE_BASE + LRFDPBE_O_RXFHRD, LAESLink_addr(&rxState.rx[slot * LAESLINK_FIFO_WORDS]),
                                   DRAIN_HALFWORDS, LAESLINK_SIZE_HALF, LAESLINK_INC_NONE, LAESLINK_INC_HALF,
                                   LAESLINK_ARB_X16, LAESLINK_MODE_BASIC));
    }

    for (uint32_t slot = 0U; slot < LAESLINK_SLOTS; slot++)
    {
        n = LAESLink_buildRx(&rxState, slot, radio);
        LAESLink_setImage(&rxState.prim[4U * slot],
                 LAESLink_sgPrimary(LAESLink_taskSpareAddr(&rxState.lists[slot][n - 1U]),
                                    LAESLink_entrySpareAddr(LAESLink_alternate(9U)), n, true));
    }
    /* The relay marks "entry in RAM" on the stage pin and kicks the list */
    n = LAESLink_buildRelay(rxState.relay, LAESLINK_RELAY_TASKS, LAESLink_addr(&c[LAESLINK_RXC_KICK9]),
                            LAESLink_addr(&c[LAESLINK_RXC_GPIO]));
    LAESLink_setImage(rxState.relayPrim,
             LAESLink_sgPrimary(LAESLink_taskSpareAddr(&rxState.relay[n - 1U]),
                                LAESLink_entrySpareAddr(LAESLink_alternate(10U)), n, true));

    LAESLink_writeEntry(LAESLink_primary(9U), LAESLink_imageTask(rxState.prim));
    LAESLink_clearEntry(LAESLink_alternate(9U));
    EVTSVTConfigureDma(EVTSVT_DMA_CH9, EVTSVT_PUB_AES_COMB);
    enable = LAESLINK_CH9;

    if (radio)
    {
        /* The drain channel's entry belongs to the RCL's SysConfig table
         * slot; its primary is written here and re-armed by the list. The
         * relay listens to its completion on DMA_DONE_COMB, which the done
         * mask publishes.
         */
        LAESLink_writeEntry(LAESLink_primary(2U), LAESLink_imageTask(rxState.prim2));
        LAESLink_writeEntry(LAESLink_primary(10U), LAESLink_imageTask(rxState.relayPrim));
        LAESLink_clearEntry(LAESLink_alternate(10U));
        EVTSVTConfigureDma(EVTSVT_DMA_CH2, EVTSVT_DMA_TRIG_LRFDTRG);
        EVTSVTConfigureDma(EVTSVT_DMA_CH10, EVTSVT_PUB_DMA_DONE_COMB);
        enable |= LAESLINK_CH2 | LAESLINK_CH10;
    }
    else
    {
        EVTSVTConfigureDma(EVTSVT_DMA_CH10, EVTSVT_PUB_NONE);
    }
    LAESLink_channelsReset(mask);
    if (radio)
    {
        LAESLink_doneMaskSet(LAESLINK_CH2);
    }

    rx.cfg          = *cfg;
    rx.source       = source;
    rx.onPacket     = onPacket;
    rx.consumeSlot  = 0U;
    rx.injectSlot   = 0U;
    rx.pending      = false;
    rx.haveAccepted = false;
    rx.lastAccepted = 0U;

    LAESLink_publish();
    uDMAEnableChannel(enable);
    return LAESLINK_STATUS_SUCCESS;
}

/*
 *  ======== LAESLink_rxInject ========
 */
bool LAESLink_rxInject(const uint32_t entry[LAESLINK_FIFO_WORDS])
{
    volatile uint32_t *dst;

    if ((rx.source != LAESLink_RxSource_Memory) || rx.pending)
    {
        return false;
    }
    dst           = &rxState.rx[rx.injectSlot * LAESLINK_FIFO_WORDS];
    rx.injectSlot = (rx.injectSlot + 1U) % LAESLINK_SLOTS;
    rx.pending    = true;
    for (uint32_t i = 0U; i < LAESLINK_FIFO_WORDS; i++)
    {
        dst[i] = entry[i];
    }
    LAESLink_publish();
    uDMARequestChannel(LAESLINK_CH9);
    return true;
}

/*
 *  ======== LAESLink_rxDoorbell ========
 */
void LAESLink_rxDoorbell(void)
{
    uint32_t j = rx.consumeSlot;
    volatile uint32_t *rec = &rxState.out[j * LAESLINK_RXO_WORDS];
    uint32_t counter = LAESLink_swap32(rec[LAESLINK_RXO_HDR]);
    LAESLink_RxStatus status;

    /* Constant-time compare, then the replay check, and nothing else */
    if (!CryptoUtils_buffersMatch(&rec[LAESLINK_RXO_MIC_CALC], &rec[LAESLINK_RXO_MIC_RX], LAESLINK_MIC_LEN))
    {
        status = LAESLink_RxStatus_MicFailed;
    }
    else if (rx.haveAccepted && (counter <= rx.lastAccepted))
    {
        status = LAESLink_RxStatus_Replay;
    }
    else
    {
        rx.haveAccepted = true;
        rx.lastAccepted = counter;
        status          = LAESLink_RxStatus_Accepted;
    }
    rx.onPacket(status, counter,
                (status == LAESLink_RxStatus_Accepted) ? (const uint8_t *)&rec[LAESLINK_RXO_PT] : NULL);
    /* The private plaintext slot is never left readable after use */
    for (uint32_t i = 0U; i < 4U; i++)
    {
        rec[LAESLINK_RXO_PT + i] = 0U;
    }
    rx.consumeSlot = (j + 1U) % LAESLINK_SLOTS;
    rx.pending     = false;
}

/*
 *  ======== LAESLink_rxClose ========
 */
void LAESLink_rxClose(void)
{
    bool radio    = (rx.source == LAESLink_RxSource_Radio);
    uint32_t mask = LAESLINK_CH9 | LAESLINK_CH10 | (radio ? LAESLINK_CH2 : 0U);

    uDMADisableChannel(mask);
    LAESLink_clearEntry(LAESLink_primary(9U));
    LAESLink_clearEntry(LAESLink_primary(10U));
    if (radio)
    {
        LAESLink_clearEntry(LAESLink_primary(2U));
    }
    LAESLink_channelsReset(mask);
    /* Channels off before abort: the LAES requires that order */
    LAESLink_laesClose();
    EVTSVTConfigureDma(EVTSVT_DMA_CH9, EVTSVT_PUB_NONE);
    EVTSVTConfigureDma(EVTSVT_DMA_CH10, EVTSVT_PUB_NONE);
    for (uint32_t i = 0U; i < LAESLINK_RXO_WORDS * LAESLINK_SLOTS; i++)
    {
        rxState.out[i] = 0U;
    }
    LAESLink_close();
}
