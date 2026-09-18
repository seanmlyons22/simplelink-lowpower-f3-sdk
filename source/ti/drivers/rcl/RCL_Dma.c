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
 *  ======== RCL_Dma.c ========
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <ti/devices/DeviceFamily.h>

#if (DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX)

#include <ti/drivers/rcl/RCL_Dma.h>
#include <ti/drivers/rcl/RCL_Buffer.h>
#include <ti/drivers/rcl/LRF.h>

#include <ti/drivers/dma/UDMALPF3.h>
#include <ti/drivers/Power.h>

#include <ti/drivers/dpl/HwiP.h>

#include DeviceFamily_constructPath(driverlib/evtsvt.h)
#include DeviceFamily_constructPath(driverlib/udma.h)
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_lrfdpbe.h)
#include DeviceFamily_constructPath(inc/hw_lrfdtxf.h)
#include DeviceFamily_constructPath(inc/hw_lrfdrxf.h)
#include DeviceFamily_constructPath(inc/hw_lrfddbell.h)

/* DMA control table entry, channel mask and EVTSVT subscriber from SysConfig */
extern volatile uDMAControlTableEntry *RCL_dmaControlTableEntry;
extern uint32_t RCL_dmaChannelMask;
extern uint32_t RCL_dmaChannelSubscriberId;

/* Transfers are byte wide against the FIFO data ports, which push or pop as
 * many bytes as the bus access is wide.
 *
 * The LRFD holds its DMA request asserted for as long as the selected FIFO
 * condition is true, and the uDMA waits for the request to fall before it
 * arbitrates again. The threshold must therefore be set so that a single
 * arbitration makes the condition false, otherwise the channel stops after one
 * burst with DMA.STATUS.STATE left at "waiting for uDMA request to clear".
 * That is what rclDmaFillThreshold computes. It also fixes how much data the
 * FIFO holds: the request is asserted while fewer than the arbitration size is
 * buffered, so the FIFO runs at roughly one arbitration of data and is topped
 * up as the radio consumes it. */
#define RCL_DMA_TX_CHUNK_DEFAULT 32U
/* The RX drain must complete in one arbitration, so it arbitrates over the
 * largest transfer the uDMA supports */
#define RCL_DMA_RX_ARB          UDMA_ARB_1024

/* ============================================================================
 * Static Global Variables
 * ============================================================================
 */

typedef struct {
    RCL_Buffer_TxBuffer *txBuffer;     /* Posted TX buffer; NULL when none */
    RCL_MultiBuffer     *rxBuffer;     /* Posted RX buffer; NULL when none */
    bool                 txArmed;      /* TX FIFO prepared by a running command */
    bool                 rxArmed;      /* RX FIFO prepared by a running command */
} RCL_DmaState;

static RCL_DmaState rclDmaState;

/* How much the TX transfer moves per arbitration, as the uDMA arbitration
 * field and in bytes. RCL_Dma_setTxChunkSize keeps the two in step. */
static uint32_t rclDmaTxArb      = UDMA_ARB_32;
static uint32_t rclDmaTxArbBytes = RCL_DMA_TX_CHUNK_DEFAULT;

/* ============================================================================
 * Forward Declarations
 * ============================================================================
 */

static uint32_t rclDmaFillThreshold(uint32_t fifoSize, uint32_t arbBytes);
static void rclDmaConfigureTrigger(uint32_t fcfg5);
static void rclDmaStartTx(RCL_Buffer_TxBuffer *txBuffer);
static uint32_t rclDmaDrainRx(RCL_MultiBuffer *rxBuffer, uint32_t numBytes);

/* ============================================================================
 * Implementations
 * ============================================================================
 */

/*
 *  ======== rclDmaFillThreshold ========
 */
/* Largest threshold that one arbitration is guaranteed to fall below */
static uint32_t rclDmaFillThreshold(uint32_t fifoSize, uint32_t arbBytes)
{
    uint32_t threshold = 1U;

    if (fifoSize > arbBytes)
    {
        threshold = (fifoSize - arbBytes) + 1U;
    }
    return threshold;
}

/*
 *  ======== rclDmaConfigureTrigger ========
 */
/* Select the FIFO condition driving the LRFD DMA trigger. These registers are
 * lost in standby, so they are written on every arm. */
static void rclDmaConfigureTrigger(uint32_t fcfg5)
{
    HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG5) = fcfg5;
    HWREG_WRITE_LRF(LRFDDBELL_BASE + LRFDDBELL_O_DMACFG) = LRFDDBELL_DMACFG_TRIGSRC_FIFO | LRFDDBELL_DMACFG_EN_ON;
}

/*
 *  ======== rclDmaStartTx ========
 */
/* Transfer the data entry (length field, pad and packet) into the TX FIFO
 * through the FIFO data port. The port pushes as many bytes as the access
 * width, so a byte wide transfer pushes one byte per access, and the FIFO
 * advances TXFWP itself. No pointer write is needed, so RCL-367 does not
 * apply. Note the PBE TXFBWR alias is not usable here: it takes the byte in a
 * 32 bit register and a byte wide bus access advances the FIFO without
 * carrying the data. */
static void rclDmaStartTx(RCL_Buffer_TxBuffer *txBuffer)
{
    uint32_t numBytes = RCL_Buffer_DataEntry_paddedLen(txBuffer->length);

    uDMASetChannelControl(RCL_dmaControlTableEntry,
                          UDMA_SRC_INC_8 | UDMA_DST_INC_NONE | UDMA_SIZE_8 | rclDmaTxArb);
    uDMASetChannelTransfer(RCL_dmaControlTableEntry,
                           UDMA_MODE_BASIC,
                           (void *) &txBuffer->length,
                           (void *) (LRFDTXF_BASE + LRFDTXF_O_TXD),
                           numBytes);
    uDMADisableChannelAttribute(RCL_dmaChannelMask, UDMA_ATTR_USEBURST);
    UDMALPF3_channelEnable(RCL_dmaChannelMask);
}

/*
 *  ======== rclDmaDrainRx ========
 */
/* Move everything PBE has committed out of the RX FIFO through the FIFO data
 * port. The FIFO advances RXFRP itself, so no pointer write is needed.
 *
 * The transfer is sized to what is committed and the arbitration is large
 * enough to cover it in one go. That is required, not an optimisation: the
 * LRFD holds the request asserted while the FIFO has readable bytes, and the
 * uDMA will not arbitrate again until it falls, so a drain that needs a second
 * arbitration would stop half way. */
static uint32_t rclDmaDrainRx(RCL_MultiBuffer *rxBuffer, uint32_t numBytes)
{
    uint32_t space = (uint32_t) rxBuffer->length - rxBuffer->tailIndex;

    if (numBytes > space)
    {
        numBytes = space;
    }
    if (numBytes > UDMA_XFER_SIZE_MAX)
    {
        numBytes = UDMA_XFER_SIZE_MAX;
    }
    if (numBytes > 0U)
    {
        uDMASetChannelControl(RCL_dmaControlTableEntry,
                              UDMA_SRC_INC_NONE | UDMA_DST_INC_8 | UDMA_SIZE_8 | RCL_DMA_RX_ARB);
        uDMASetChannelTransfer(RCL_dmaControlTableEntry,
                               UDMA_MODE_BASIC,
                               (void *) (LRFDRXF_BASE + LRFDRXF_O_RXD),
                               RCL_MultiBuffer_getNextWritableByte(rxBuffer),
                               numBytes);
        uDMADisableChannelAttribute(RCL_dmaChannelMask, UDMA_ATTR_USEBURST);
        UDMALPF3_channelEnable(RCL_dmaChannelMask);

        /* The transfer is one arbitration against a FIFO that already holds the
         * data, so it completes in a bounded number of bus cycles. The limit is
         * only there so that a stalled channel cannot hang the handler. */
        uint32_t timeout = 4U * UDMA_XFER_SIZE_MAX;
        while ((uDMAGetChannelMode(RCL_dmaControlTableEntry) != UDMA_MODE_STOP) && (timeout > 0U))
        {
            timeout--;
        }
        UDMALPF3_channelDisable(RCL_dmaChannelMask);

        numBytes -= uDMAGetChannelSize(RCL_dmaControlTableEntry);
        RCL_MultiBuffer_commitBytes(rxBuffer, numBytes);
        /* RXFRP was moved by the FIFO data port */
        LRF_clearRxFifoDeallocated();
    }
    return numBytes;
}

/*
 *  ======== RCL_Dma_setTxChunkSize ========
 */
int_fast16_t RCL_Dma_setTxChunkSize(uint32_t numBytes)
{
    int_fast16_t status = RCL_Dma_Status_Error_Param;

    /* The uDMA encodes the arbitration size as a power of two, and the FIFO
     * threshold is derived from it, so only those sizes can be set. */
    if ((numBytes >= 2U) && (numBytes <= (uint32_t) UDMA_XFER_SIZE_MAX) &&
        ((numBytes & (numBytes - 1U)) == 0U))
    {
        uint32_t shift = 0U;

        while ((1U << shift) < numBytes)
        {
            shift++;
        }

        uintptr_t key = HwiP_disable();
        rclDmaTxArb = shift << UDMA_ARB_S;
        rclDmaTxArbBytes = numBytes;
        HwiP_restore(key);

        status = RCL_Dma_Status_Success;
    }

    return status;
}

/*
 *  ======== RCL_Dma_open ========
 */
void RCL_Dma_open(void)
{
    UDMALPF3_init();
    Power_setDependency(PowerLPF3_PERIPH_DMA);

    EVTSVTConfigureDma(RCL_dmaChannelSubscriberId, EVTSVT_DMA_TRIG_LRFDTRG);
    /* Only the primary control structure is used */
    UDMALPF3_disableAttribute(RCL_dmaChannelMask, UDMA_ATTR_ALTSELECT);

    rclDmaState.txBuffer = NULL;
    rclDmaState.rxBuffer = NULL;
    rclDmaState.txArmed = false;
    rclDmaState.rxArmed = false;
}

/*
 *  ======== RCL_Dma_close ========
 */
void RCL_Dma_close(void)
{
    HWREG_WRITE_LRF(LRFDDBELL_BASE + LRFDDBELL_O_DMACFG) = 0U;
    UDMALPF3_channelDisable(RCL_dmaChannelMask);
    Power_releaseDependency(PowerLPF3_PERIPH_DMA);
}

/*
 *  ======== RCL_Dma_putTxBuffer ========
 */
int_fast16_t RCL_Dma_putTxBuffer(RCL_Buffer_TxBuffer *txBuffer)
{
    int_fast16_t status = RCL_Dma_Status_Success;

    if ((txBuffer == NULL) || (RCL_Buffer_DataEntry_paddedLen(txBuffer->length) > UDMA_XFER_SIZE_MAX))
    {
        status = RCL_Dma_Status_Error_Param;
    }
    else
    {
        uintptr_t key = HwiP_disable();
        if (rclDmaState.txBuffer != NULL)
        {
            status = RCL_Dma_Status_Error_Busy;
        }
        else
        {
            txBuffer->state = RCL_BufferStateInUse;
            rclDmaState.txBuffer = txBuffer;
            if (rclDmaState.txArmed)
            {
                rclDmaStartTx(txBuffer);
            }
        }
        HwiP_restore(key);
    }

    return status;
}

/*
 *  ======== RCL_Dma_putRxBuffer ========
 */
int_fast16_t RCL_Dma_putRxBuffer(RCL_MultiBuffer *rxBuffer)
{
    int_fast16_t status = RCL_Dma_Status_Success;

    if (rxBuffer == NULL)
    {
        status = RCL_Dma_Status_Error_Param;
    }
    else
    {
        uintptr_t key = HwiP_disable();
        {
            rxBuffer->state = RCL_BufferStateInUse;
            rclDmaState.rxBuffer = rxBuffer;
            if (rclDmaState.rxArmed)
            {
                LRF_setRxFifoEffSz((uint32_t) rxBuffer->length - rxBuffer->tailIndex);
            }
        }
        HwiP_restore(key);
    }

    return status;
}

/*
 *  ======== RCL_Dma_armTx ========
 */
/*
 *  ======== RCL_Dma_armTx ========
 */
void RCL_Dma_armTx(void)
{
    /* The FIFO frees space as the modulator consumes it, which is what paces
     * the refill. LRF_prepareTxFifo leaves auto deallocate off because the CPU
     * path retries the FIFO to repeat a packet; the DMA path does not. */
    HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0) =
        HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0) | LRFDPBE_FCFG0_TXADEAL_M;

    /* Nothing has been written since the FIFO was prepared, so this is its size */
    uint32_t fifoSize = HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_TXFWRITABLE);

    rclDmaConfigureTrigger(LRFDPBE_FCFG5_DMAREQ_TXWRBTHR_MET | LRFDPBE_FCFG5_DMASREQ_NONE);
    HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_TXFWBTHRS) = rclDmaFillThreshold(fifoSize, rclDmaTxArbBytes);

    uintptr_t key = HwiP_disable();
    rclDmaState.txArmed = true;
    if (rclDmaState.txBuffer != NULL)
    {
        rclDmaStartTx(rclDmaState.txBuffer);
    }
    HwiP_restore(key);
}

/*
 *  ======== RCL_Dma_armRx ========
 */
void RCL_Dma_armRx(void)
{
    /* FCFG0 is left as LRF_prepareRxFifo set it. Auto deallocate must stay off:
     * space is freed by the LRF_setRxFifoEffSz calls below, and letting the
     * hardware move RXFSRP as well makes those writes move it a full round. */

    /* Request while the FIFO holds anything readable */
    rclDmaConfigureTrigger(LRFDPBE_FCFG5_DMAREQ_RXRDBTHR_MET | LRFDPBE_FCFG5_DMASREQ_NONE);
    HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_RXFRBTHRS) = 1U;

    uintptr_t key = HwiP_disable();
    rclDmaState.rxArmed = true;
    RCL_MultiBuffer *rxBuffer = rclDmaState.rxBuffer;
    HwiP_restore(key);

    /* Hold PBE to what the posted buffer can take */
    if (rxBuffer != NULL)
    {
        LRF_setRxFifoEffSz((uint32_t) rxBuffer->length - rxBuffer->tailIndex);
    }
    else
    {
        LRF_setRxFifoEffSz(0U);
    }
}

/*
 *  ======== RCL_Dma_finishRx ========
 */
uint32_t RCL_Dma_finishRx(void)
{
    uint32_t numBytes = 0U;
    RCL_MultiBuffer *rxBuffer = rclDmaState.rxBuffer;

    if (rclDmaState.rxArmed && (rxBuffer != NULL))
    {
        numBytes = rclDmaDrainRx(rxBuffer, HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_RXFREADABLE));
        LRF_setRxFifoEffSz((uint32_t) rxBuffer->length - rxBuffer->tailIndex);
    }

    return numBytes;
}

/*
 *  ======== RCL_Dma_stop ========
 */
void RCL_Dma_stop(void)
{
    /* Take the trigger away before the channel is released. The selected FIFO
     * condition is true again as soon as the radio has drained the FIFO, so the
     * request would stay asserted with no transfer left to serve, and the uDMA
     * waits for a request to fall before it arbitrates again. Leaving it
     * asserted parks the channel in that wait and the next command never gets
     * its data. */
    HWREG_WRITE_LRF(LRFDDBELL_BASE + LRFDDBELL_O_DMACFG) = 0U;
    HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG5) = LRFDPBE_FCFG5_DMAREQ_NONE |
                                                      LRFDPBE_FCFG5_DMASREQ_NONE;

    uintptr_t key = HwiP_disable();
    UDMALPF3_channelDisable(RCL_dmaChannelMask);
    rclDmaState.txArmed = false;
    rclDmaState.rxArmed = false;
    RCL_Buffer_TxBuffer *txBuffer = rclDmaState.txBuffer;
    rclDmaState.txBuffer = NULL;
    HwiP_restore(key);

    if (txBuffer != NULL)
    {
        txBuffer->state = RCL_BufferStateFinished;
    }
}

#endif /* DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX */
