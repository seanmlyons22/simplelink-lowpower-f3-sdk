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
 *  ======== LAESLink_lists.c ========
 *  Task-list construction for the transmit and receive datapaths.
 *
 *  The list is a peripheral scatter-gather list (b110 primary) with mixed
 *  task modes. A task that must run immediately after its predecessor is a
 *  memory scatter-gather task (cycle_ctrl = b101), which auto-requests, so the
 *  list continues with no request from outside. A task that must wait for
 *  the LAES is a peripheral scatter-gather task (b111), which stops the list;
 *  the next request is the AESDONE edge.
 *
 *  An earlier design placed a kick task, a one-word write of the channel's
 *  own bit into DMA.SOFTREQ, after every task that had to continue. That
 *  cannot work. The controller advances one list entry per request, so an
 *  entry consumes one request and a kick produces one, which makes a kick
 *  request-neutral: a list with R real tasks always needs R requests from
 *  outside however many kicks it holds. Silicon gate T4 measures the
 *  mixed-mode behaviour this file relies on. Kicks remain correct across
 *  channels, which is what the relay lists use them for.
 */

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_aes.h)
#include DeviceFamily_constructPath(inc/hw_dma.h)
#include DeviceFamily_constructPath(inc/hw_gpio.h)
#include DeviceFamily_constructPath(inc/hw_lrfdpbe.h)
#include DeviceFamily_constructPath(inc/hw_lrfdtxf.h)

#include <ti/drivers/dpl/DebugP.h>
#include <ti/drivers/laeslink/LAESLink_ccm.h>
#include <ti/drivers/laeslink/LAESLink_lists.h>

/* The lists hold absolute addresses, so this is the flat, non-TFM map */
_Static_assert(AES_BASE == 0x400C0000U, "flat AES base");
_Static_assert(DMA_BASE == 0x400C4000U, "flat DMA base");
_Static_assert(GPIO_BASE == 0x40023000U, "flat GPIO base");
_Static_assert(LRFDTXF_BASE + LRFDTXF_O_TXD == 0x40081800U, "TX FIFO word port");
_Static_assert(LRFDPBE_BASE + LRFDPBE_O_API == 0x40081030U, "PBE API register");

#define AES_REG(offset)     (AES_BASE + (offset))
#define DMA_REG(offset)     (DMA_BASE + (offset))
#define GPIO_REG(offset)    (GPIO_BASE + (offset))
#define LRFD_TXD            (LRFDTXF_BASE + LRFDTXF_O_TXD)
#define LRFD_API            (LRFDPBE_BASE + LRFDPBE_O_API)

/*
 *  ======== LAESLink_listBegin ========
 */
void LAESLink_listBegin(LAESLink_ListBuilder *b, volatile LAESLink_Task *list, uint32_t capacity)
{
    b->list     = list;
    b->capacity = capacity;
    b->n        = 0U;
}

/*
 *  ======== LAESLink_listPush ========
 */
void LAESLink_listPush(LAESLink_ListBuilder *b, LAESLink_Task task, LAESLink_Flow flow)
{
    LAESLink_Task t;

    DebugP_assert(b->n < b->capacity);
    DebugP_assert((task.control & UDMA_MODE_M) == LAESLINK_MODE_PER_SG_ALT);

    t = LAESLink_withMode(task, (flow == LAESLink_Flow_Continue) ? LAESLINK_MODE_MEM_SG_ALT : LAESLINK_MODE_PER_SG_ALT);

    b->list[b->n].srcEnd  = t.srcEnd;
    b->list[b->n].dstEnd  = t.dstEnd;
    b->list[b->n].control = t.control;
    b->list[b->n].spare   = t.spare;
    b->n++;
}

/*
 *  ======== LAESLink_word ========
 */
LAESLink_Task LAESLink_word(uint32_t src, uint32_t dst)
{
    return LAESLink_transfer(src, dst, 1U, LAESLINK_SIZE_WORD, LAESLINK_INC_NONE, LAESLINK_INC_NONE, LAESLINK_ARB_X1,
                             LAESLINK_MODE_PER_SG_ALT);
}

/*
 *  ======== LAESLink_block ========
 */
LAESLink_Task LAESLink_block(uint32_t src, uint32_t dst)
{
    return LAESLink_transfer(src, dst, 4U, LAESLINK_SIZE_WORD, LAESLINK_INC_WORD, LAESLINK_INC_WORD, LAESLINK_ARB_X4,
                             LAESLINK_MODE_PER_SG_ALT);
}

/*
 *  ======== LAESLink_blockToFifo ========
 */
LAESLink_Task LAESLink_blockToFifo(uint32_t src, uint32_t dst)
{
    return LAESLink_transfer(src, dst, 4U, LAESLINK_SIZE_WORD, LAESLINK_INC_WORD, LAESLINK_INC_NONE, LAESLINK_ARB_X4,
                             LAESLINK_MODE_PER_SG_ALT);
}

/*
 *  ======== LAESLink_halfwords ========
 */
LAESLink_Task LAESLink_halfwords(uint32_t src, uint32_t dst)
{
    return LAESLink_transfer(src, dst, 2U, LAESLINK_SIZE_HALF, LAESLINK_INC_HALF, LAESLINK_INC_HALF, LAESLINK_ARB_X2,
                             LAESLINK_MODE_PER_SG_ALT);
}

/*
 *  ======== sinkWord ========
 *  Where word index of the entry for slot goes: the FIFO port, or the capture
 *  array.
 */
static uint32_t sinkWord(LAESLink_TxSink sink, const LAESLink_TxState *state, uint32_t slot, uint32_t index)
{
    if (sink == LAESLink_TxSink_RadioFifo)
    {
        return LRFD_TXD;
    }
    return LAESLink_addr(&state->capture[slot * LAESLINK_FIFO_WORDS + index]);
}

/*
 *  ======== LAESLink_buildTx ========
 */
uint32_t LAESLink_buildTx(LAESLink_TxState *state, uint32_t slot, LAESLink_TxSink sink)
{
    LAESLink_ListBuilder b;
    const volatile uint32_t *consts = state->consts;
    uint32_t pt   = LAESLink_addr(&state->slots[4U * slot]);
    uint32_t pc   = LAESLink_addr(&state->a0[LAESLINK_CCM_COUNTER_WORD]);
    uint32_t next = (slot + 1U) % LAESLINK_SLOTS;
    uint32_t ctDst = sinkWord(sink, state, slot, 2U);

    LAESLink_listBegin(&b, state->lists[slot], LAESLINK_TX_LIST_CAP);

    /* The FIFO entry opens with the length word and the cleartext header,
     * which is this packet's counter. Both are read here, before the S0
     * operation overwrites A0 word 1 with the next counter, and they are
     * still the first two words the sink sees.
     */
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_TXC_LEN]), sinkWord(sink, state, slot, 0U)),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_word(pc, sinkWord(sink, state, slot, 1U)), LAESLink_Flow_Continue);
    /* S0 = AES(A0); the CTR64 increment leaves BUF = A0 with counter + 1 */
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_TXC_CFG_S0]), AES_REG(AES_O_AUTOCFG)),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_block(LAESLink_addr(state->a0), AES_REG(AES_O_BUF0)), LAESLink_Flow_Wait);
    LAESLink_listPush(&b, LAESLink_word(AES_REG(AES_O_TXT0), LAESLink_addr(state->s0)), LAESLink_Flow_Continue);
    /* The advanced counter goes straight into the A0 image, which this packet
     * has finished with. A0 is the counter of record from here to the end of
     * the list.
     */
    LAESLink_listPush(&b, LAESLink_word(AES_REG(AES_O_BUF1), pc), LAESLink_Flow_Continue);
    /* S1 = AES(A1) */
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_TXC_CFG_S1]), AES_REG(AES_O_AUTOCFG)),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_block(LAESLink_addr(state->a1), AES_REG(AES_O_BUF0)), LAESLink_Flow_Wait);
    /* Ciphertext: the hardware XOR gives TXT = S1 ^ P, copied out of TXT
     * straight into the sink, never into SRAM on the radio path.
     */
    LAESLink_listPush(&b, LAESLink_block(pt, AES_REG(AES_O_TXTX0)), LAESLink_Flow_Continue);
    LAESLink_listPush(&b,
                      (sink == LAESLink_TxSink_RadioFifo) ? LAESLink_blockToFifo(AES_REG(AES_O_TXT0), ctDst)
                                                          : LAESLink_block(AES_REG(AES_O_TXT0), ctDst),
                      LAESLink_Flow_Continue);
    /* CBC-MAC over B0, B1, P. B0 still runs under CFG_S1, where AESSRC is
     * BUF, so the engine computes AES(B0), which is X1 because the CBC-MAC IV
     * is zero. TXT needs no clear.
     */
    LAESLink_listPush(&b, LAESLink_block(LAESLink_addr(state->b0), AES_REG(AES_O_BUF0)), LAESLink_Flow_Wait);
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_TXC_CFG_MAC]), AES_REG(AES_O_AUTOCFG)),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_block(LAESLink_addr(state->b1), AES_REG(AES_O_BUF0)), LAESLink_Flow_Wait);
    LAESLink_listPush(&b, LAESLink_block(pt, AES_REG(AES_O_BUF0)), LAESLink_Flow_Wait);
    /* MIC = (T ^ S0)[0..4] through the TXTX hardware XOR, then to the sink;
     * the packet is complete.
     */
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(state->s0), AES_REG(AES_O_TXTX0)), LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_word(AES_REG(AES_O_TXT0), sinkWord(sink, state, slot, 6U)),
                      LAESLink_Flow_Continue);
    /* Post the operation. This is a later task than the last TXD write on
     * the same channel and the FIFO auto-commits every pushed word, so the
     * entry is complete and committed when the PBE pops its header. The
     * memory sink has nothing to post to.
     */
    if (sink == LAESLink_TxSink_RadioFifo)
    {
        LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_TXC_API]), LRFD_API),
                          LAESLink_Flow_Continue);
    }
    /* Next packet's counter from A0 into the other three images. The A1
     * write is the completion signal the watchdog reads: it happens only
     * after the ciphertext and MIC have reached the sink.
     */
    LAESLink_listPush(&b, LAESLink_word(pc, LAESLink_addr(&state->a1[LAESLINK_CCM_COUNTER_WORD])),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_word(pc, LAESLink_addr(&state->b0[LAESLINK_CCM_COUNTER_WORD])),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_halfwords(pc, LAESLink_addr(state->b1) + LAESLINK_CCM_B1_COUNTER_OFFSET),
                      LAESLink_Flow_Continue);
    /* One edge on the stage pin per packet, driven by the DMA itself */
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_TXC_GPIO]), GPIO_REG(GPIO_O_DOUTTGL31_0)),
                      LAESLink_Flow_Continue);
    /* Refresh the relay primary (channel 9) and then our own primary for the
     * next slot, and stop until the next packet request. Both point at the
     * next slot: the relay group and the packet list advance together.
     */
    LAESLink_listPush(&b,
                      LAESLink_block(LAESLink_addr(&state->relayPrim[4U * next]),
                                     LAESLink_entryStartAddr(LAESLink_primary(9U))),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b,
                      LAESLink_block(LAESLink_addr(&state->prim[4U * next]),
                                     LAESLink_entryStartAddr(LAESLink_primary(8U))),
                      LAESLink_Flow_Wait);
    return b.n;
}

/*
 *  ======== LAESLink_buildRelay ========
 */
uint32_t LAESLink_buildRelay(volatile LAESLink_Task *list, uint32_t n, uint32_t kickSrc, uint32_t gpioSrc)
{
    LAESLink_ListBuilder b;
    LAESLink_Task kick = LAESLink_word(kickSrc, DMA_REG(DMA_O_SOFTREQ));

    LAESLink_listBegin(&b, list, n);
    while (b.n < n)
    {
        if (gpioSrc != 0U)
        {
            LAESLink_listPush(&b, LAESLink_word(gpioSrc, GPIO_REG(GPIO_O_DOUTTGL31_0)), LAESLink_Flow_Continue);
        }
        LAESLink_listPush(&b, kick, LAESLink_Flow_Wait);
    }
    return b.n;
}

/*
 *  ======== LAESLink_buildTxRelay ========
 *  One group of the transmit relay (LAES design 6.5). The sample channel
 *  runs in basic mode, so it completes and disables itself; the group puts
 *  it back and takes the peripheral's event down before the packet list
 *  starts. The housekeeping rows sit here rather than at the head of the
 *  packet list so that the channel is listening to the sample source again
 *  before the AES pipeline begins.
 */
uint32_t LAESLink_buildTxRelay(LAESLink_TxState *state, uint32_t slot, uint32_t ackRegister)
{
    LAESLink_ListBuilder b;
    const volatile uint32_t *consts = state->consts;
    uint32_t bit  = LAESLink_addr(&consts[LAESLINK_TXC_BIT_SAMPLE]);
    uint32_t next = (slot + 1U) % LAESLINK_SLOTS;

    LAESLink_listBegin(&b, &state->relay[LAESLINK_TX_RELAY_TASKS * slot], LAESLINK_TX_RELAY_TASKS);
    /* Clear the sample channel's done flag, so that its next completion is a
     * fresh edge on DMA_DONE_COMB.
     */
    LAESLink_listPush(&b, LAESLink_word(bit, DMA_REG(DMA_O_REQDONE)), LAESLink_Flow_Continue);
    /* Re-arm it into the next slot, never the one about to be encrypted */
    LAESLink_listPush(&b,
                      LAESLink_block(LAESLink_addr(&state->primSample[4U * next]),
                                     LAESLink_entryStartAddr(LAESLink_primary(LAESLINK_TX_SAMPLE_CH))),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_word(bit, DMA_REG(DMA_O_SETCHANNELEN)), LAESLink_Flow_Continue);
    /* Take the peripheral's event down, with the channel already armed
     * behind it. The event is latched, so until it is cleared the fabric
     * sees no new edge and the next sample would never be asked for; and
     * the sample it acknowledges is already out of the peripheral's FIFO,
     * so nothing raises it again until the next one arrives.
     */
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_TXC_ACK]), ackRegister),
                      LAESLink_Flow_Continue);
    /* Start the packet, then wait for the next sample */
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_TXC_KICK8]), DMA_REG(DMA_O_SOFTREQ)),
                      LAESLink_Flow_Wait);
    return b.n;
}

/*
 *  ======== LAESLink_buildRx ========
 */
uint32_t LAESLink_buildRx(LAESLink_RxState *state, uint32_t slot, bool radio)
{
    LAESLink_ListBuilder b;
    const volatile uint32_t *consts = state->consts;
    const volatile uint32_t *rx  = &state->rx[slot * LAESLINK_FIFO_WORDS];
    const volatile uint32_t *out = &state->out[slot * LAESLINK_RXO_WORDS];
    uint32_t hdr  = LAESLink_addr(&rx[LAESLINK_RXI_HDR]);
    uint32_t pt   = LAESLink_addr(&out[LAESLINK_RXO_PT]);
    uint32_t next = (slot + 1U) % LAESLINK_SLOTS;

    LAESLink_listBegin(&b, state->lists[slot], LAESLINK_RX_LIST_CAP);

    if (radio)
    {
        /* Acknowledge channel 2, so that its next completion makes a fresh
         * edge, and re-arm it into the next slot, not the one about to be
         * decrypted.
         */
        LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_RXC_BIT2]), DMA_REG(DMA_O_REQDONE)),
                          LAESLink_Flow_Continue);
        LAESLink_listPush(&b,
                          LAESLink_block(LAESLink_addr(&state->prim2[4U * next]),
                                         LAESLink_entryStartAddr(LAESLink_primary(2U))),
                          LAESLink_Flow_Continue);
        LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_RXC_BIT2]), DMA_REG(DMA_O_SETCHANNELEN)),
                          LAESLink_Flow_Continue);
    }
    /* Header (counter) into the images */
    LAESLink_listPush(&b, LAESLink_word(hdr, LAESLink_addr(&state->a0[LAESLINK_CCM_COUNTER_WORD])),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_word(hdr, LAESLink_addr(&state->a1[LAESLINK_CCM_COUNTER_WORD])),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_word(hdr, LAESLink_addr(&state->b0[LAESLINK_CCM_COUNTER_WORD])),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_halfwords(hdr, LAESLink_addr(state->b1) + LAESLINK_CCM_B1_COUNTER_OFFSET),
                      LAESLink_Flow_Continue);
    /* S0 and S1 under one configuration: the free-running BUF3 trigger needs
     * no re-arm between them, and it carries on into the first CBC-MAC block
     * below. The counter is never enabled on receive.
     */
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_RXC_CFG_S]), AES_REG(AES_O_AUTOCFG)),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_block(LAESLink_addr(state->a0), AES_REG(AES_O_BUF0)), LAESLink_Flow_Wait);
    LAESLink_listPush(&b, LAESLink_word(AES_REG(AES_O_TXT0), LAESLink_addr(state->s0)), LAESLink_Flow_Continue);
    /* S1, then P = C ^ S1 into the private output slot */
    LAESLink_listPush(&b, LAESLink_block(LAESLink_addr(state->a1), AES_REG(AES_O_BUF0)), LAESLink_Flow_Wait);
    LAESLink_listPush(&b, LAESLink_block(LAESLink_addr(&rx[LAESLINK_RXI_CT]), AES_REG(AES_O_TXTX0)),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_block(AES_REG(AES_O_TXT0), pt), LAESLink_Flow_Continue);
    /* CBC-MAC over B0, B1, P. As on transmit, B0 runs under CFG_S and needs
     * no TXT clear.
     */
    LAESLink_listPush(&b, LAESLink_block(LAESLink_addr(state->b0), AES_REG(AES_O_BUF0)), LAESLink_Flow_Wait);
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_RXC_CFG_MAC]), AES_REG(AES_O_AUTOCFG)),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_block(LAESLink_addr(state->b1), AES_REG(AES_O_BUF0)), LAESLink_Flow_Wait);
    LAESLink_listPush(&b, LAESLink_block(pt, AES_REG(AES_O_BUF0)), LAESLink_Flow_Wait);
    /* Expected MIC, received MIC and header into the output record */
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(state->s0), AES_REG(AES_O_TXTX0)), LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_word(AES_REG(AES_O_TXT0), LAESLink_addr(&out[LAESLINK_RXO_MIC_CALC])),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&rx[LAESLINK_RXI_MIC]), LAESLink_addr(&out[LAESLINK_RXO_MIC_RX])),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_word(hdr, LAESLink_addr(&out[LAESLINK_RXO_HDR])), LAESLink_Flow_Continue);
    /* Doorbell: set the pin's bit in the GPIO raw interrupt status, which
     * raises the GPIO interrupt for that DIO directly and is the one ISR per
     * packet; then toggle the same pin so the scope sees the same instant.
     * This keeps DMA_DONE_COMB carrying only the channel 2 drain.
     */
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_RXC_DOORBELL]), GPIO_REG(GPIO_O_ISET)),
                      LAESLink_Flow_Continue);
    LAESLink_listPush(&b, LAESLink_word(LAESLink_addr(&consts[LAESLINK_RXC_DOORBELL]), GPIO_REG(GPIO_O_DOUTTGL31_0)),
                      LAESLink_Flow_Continue);
    if (radio)
    {
        LAESLink_listPush(&b,
                          LAESLink_block(LAESLink_addr(state->relayPrim),
                                         LAESLink_entryStartAddr(LAESLink_primary(10U))),
                          LAESLink_Flow_Continue);
    }
    LAESLink_listPush(&b,
                      LAESLink_block(LAESLink_addr(&state->prim[4U * next]),
                                     LAESLink_entryStartAddr(LAESLink_primary(9U))),
                      LAESLink_Flow_Wait);
    return b.n;
}
