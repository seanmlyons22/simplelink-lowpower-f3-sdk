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
#include DeviceFamily_constructPath(inc/hw_lrfddbell.h)
#include DeviceFamily_constructPath(inc/hw_gpio.h)

/* DMA control table entry, channel mask and EVTSVT subscriber from SysConfig */
extern volatile uDMAControlTableEntry *RCL_dmaControlTableEntry;
extern uint32_t RCL_dmaChannelMask;
extern uint32_t RCL_dmaChannelSubscriberId;

/* A second channel, its control table entry and EVTSVT subscriber, also from
 * SysConfig. The LRF trigger reaches two of the DMA channels and the data path
 * uses one of them; this is the other. SysConfig emits these only for an
 * application built with RCL_DMA_DEBUG defined, so they are weak: in any other
 * application they resolve to address 0, the second channel is never touched
 * and the data path runs exactly as it does without this. */
extern volatile uDMAControlTableEntry *RCL_dmaDebugControlTableEntry __attribute__((weak));
extern uint32_t RCL_dmaDebugChannelMask __attribute__((weak));
extern uint32_t RCL_dmaDebugChannelSubscriberId __attribute__((weak));

/* Whether the application reserved the second channel */
#define RCL_DMA_DEBUG_PRESENT()  (&RCL_dmaDebugChannelMask != NULL)

/* Transfers are halfword wide against the FIFO data ports, which push or pop
 * two bytes on every access whatever the width of the bus access.
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
/* The RX burst is taken out of the FIFO one entry at a time while it is on
 * the air, through LRFDPBE.RXFHRD and never through the LRFDRXF alias region.
 *
 * The two are different access paths in the FIFO controller and only the
 * alias region carries the RCL-367 defect: a FIFO command written to
 * LRFDPBE.FCMD is latched and takes effect one clock cycle later, and an
 * access to LRFDRXF or LRFDTXF in that cycle merges with it, on which one of
 * the two is dropped. On receive it is the pop that disappears, so the read
 * pointer does not advance and the reader gets the previous halfword again or
 * data that did not come from the FIFO at all. On an LP_EM_CC2755P20 a uDMA
 * popping LRFDRXF during a burst lost about three bursts in a thousand. The
 * erratum says in as many words that DMA cannot be used to read or write the
 * FIFO through those regions, that no silicon change is planned and that they
 * are to be removed from the documentation.
 *
 * RXFHRD is a combinational address decode into the same FIFO command process
 * rather than a latched command, so a port access and a FIFO command in the
 * same cycle resolve with the port access winning. That, and not any claim
 * that the two never meet, is what makes this safe: the PBE issues a second
 * FIFO command a few instructions after the commit, a TX FIFO retry or
 * dealloc, and it falls inside the arbitration the commit itself triggered.
 * The port access wins that one, so it is the FIFO command that is dropped,
 * and in a receive burst a TX FIFO command has nothing to do. The port pops
 * two bytes and advances RXFRP by two on every access, whatever the width of
 * the bus access, so the FIFO frees its own space as it is read.
 *
 * The LRFD request is consumed as an edge: each time it rises the uDMA
 * arbitrates exactly once and does not arbitrate again until the request has
 * fallen and risen. That was measured; armed as a level ("RXFREADABLE >= 1")
 * over a five entry burst the channel moved one item and stopped, because the
 * level never fell. A level is therefore only usable where one arbitration is
 * guaranteed to take it down, which is what rclDmaFillThreshold arranges on
 * transmit. On receive the event is a pulse instead, FCFG5.DMAREQ =
 * RXFIFO_COMMIT, one edge per committed entry, so the arbitration has to be
 * one whole entry and the entry a power of two bytes.
 *
 * Reading the port more often than the FIFO has committed bytes would return
 * a halfword it does not have, which is why the arbitration may never exceed
 * what one commit makes readable. */
/* Bound on the spin that samples the DMA response latency when a TX transfer
 * is kicked off, in loop iterations */
#define RCL_DMA_TX_KICK_SPINS   32U
/* Bound on the wait for the first entry of a TX transfer to be in the FIFO
 * when a command is armed, in loop iterations; several times the response
 * latency, so that only a channel that is not answering fails it */
#define RCL_DMA_TX_ARM_SPINS    256U
/* Iterations given to the channel to finish an arbitration that was under way
 * when the trigger was taken away, before its count is read. At the end of a
 * command the last arbitration is long over, a packet time or more; the spin
 * only covers a trigger taken away in the middle of one, and one arbitration
 * is at most 32 bus transfers. */
#define RCL_DMA_SETTLE_SPINS    8U

/* ============================================================================
 * Static Global Variables
 * ============================================================================
 */

typedef struct {
    const void          *txSource;     /* First posted TX entry, at its length field; NULL when none */
    uint32_t             txStride;     /* Distance between posted entries in bytes */
    uint32_t             txNumBytes;   /* Bytes the TX transfer moves: all posted entries */
    uint32_t             txHeaderBytes;/* Length field, numPad and pad of one entry */
    uint32_t             txLastEntries;/* Entries of the last transmission, all taken, reported until the next post */
    RCL_MultiBuffer     *rxBuffer;     /* Posted RX buffer; NULL when none */
    uint8_t             *rxBurstDst;   /* Where an RX burst's entries land */
    uint32_t             rxBurstEntry; /* Bytes of one entry of the RX burst */
    uint32_t             rxBurstStreamBytes; /* Bytes the streaming RX transfer was armed for; 0 when not streaming */
    bool                 txArmed;      /* TX FIFO prepared by a running command */
    bool                 txStarted;    /* TX transfer programmed and enabled */
    bool                 rxArmed;      /* RX FIFO prepared by a running command */
    bool                 rxBurstArmed; /* RX FIFO holds a running burst to be drained when it ends */
} RCL_DmaState;

static RCL_DmaState rclDmaState;

/* How much the TX transfer moves per arbitration, as the uDMA arbitration
 * field and in bytes, both derived from the entry size when a burst is
 * posted. */
static uint32_t rclDmaTxArb      = 0U;
static uint32_t rclDmaTxArbBytes = 0U;

/* ============================================================================
 * Forward Declarations
 * ============================================================================
 */

static uint32_t rclDmaFillThreshold(uint32_t fifoSize, uint32_t arbBytes);
static void rclDmaConfigureTrigger(uint32_t fcfg5);
static void rclDmaQuiesce(void);
static bool rclDmaStartTx(const void *source, uint32_t numBytes);
static int_fast16_t rclDmaPostTx(const void *source, uint32_t numBytes, uint32_t stride);
static void rclDmaRxAllowSpace(void);
static void rclDmaDebugArm(uint32_t numRequests);

/* Counters for faults that are otherwise silent, read with a debugger.
 * A TX underflow is the only thing that separates a packet the PBE sent from
 * the FIFO from one it sent from whatever the empty FIFO returned, since the
 * PBE never checks the TX FIFO fill level itself. */
volatile uint32_t rclDmaTxUnderrun = 0U;
volatile uint32_t rclDmaTxOverflow = 0U;
volatile uint32_t rclDmaRxOverflow = 0U;
/* Bursts whose committed bytes were not a whole number of entries when the
 * command ended, with the odd remainder. The PBE commits whole entries, so
 * this can only be a FIFO read that went wrong, which is what it is here to
 * catch. */
volatile uint32_t rclDmaRxBurstStray = 0U;
volatile uint32_t rclDmaRxBurstStrayBytes = 0U;

/* Bit of the named pin in GPIO.DOUTTGL31_0, and the source of the write that
 * toggles it. Read by the uDMA, not by the CPU. */
static volatile uint32_t rclDmaDebugPinMask = 0U;

/* Bursts armed with no pin named, or too long for one transfer, and so run
 * with the pin left alone. A trace with fewer edges than packets is explained
 * here. */
volatile uint32_t rclDmaDebugUnavailable = 0U;

/* ============================================================================
 * Implementations
 * ============================================================================
 */

/*
 *  ======== rclDmaFillThreshold ========
 */
/* Largest threshold that one arbitration is guaranteed to fall below.
 *
 * The request is the level "TXFWRITABLE >= TXFWBTHRS". One arbitration of
 * arbBytes reduces the writable count by arbBytes, and the count is at most
 * the FIFO size, so it is below the threshold after any single arbitration
 * exactly when fifoSize - arbBytes < TXFWBTHRS. */
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
 *  ======== rclDmaDebugArm ========
 */
/* Arm the second channel to toggle the debug pin once per FIFO request.
 *
 * The LRF trigger reaches two DMA channels. The data path owns one of them;
 * this arms the other on the same trigger, with a transfer that writes a
 * single word to GPIO.DOUTTGL31_0 per arbitration. The CPU does nothing in
 * between, which is the point of measuring it this way: an instrument the CPU
 * had to service would be reporting on itself rather than on the data path.
 * The toggle register is not in the LRF, so none of the FIFO access rules
 * reach it, and the data path is left exactly as it runs without this.
 *
 * One request is answered with exactly one arbitration on every channel
 * listening, so the pin changes state once per entry in both directions.
 * Measured on both sides by arming this channel with 1024 over a five packet
 * burst and taking the largest number it ever consumed in one: five.
 *
 * That holds on transmit only because the channel is armed here, inside the
 * window where the trigger is configured and TXFWBTHRS has been written, and
 * is disabled again when the command ends. The transmit trigger is the level
 * FCFG5.DMAREQ = TXWRBTHR_MET, and only the data transfer can take a level
 * down; a channel left enabled outside that window sees the level asserted
 * against a stale threshold and free runs. Receive is a pulse instead,
 * FCFG5.DMAREQ = RXFIFO_COMMIT, one edge per committed entry, and is not
 * exposed to this.
 *
 * Neither direction waits on data: the transfer is armed over the whole burst
 * before the command is submitted, so an entry written late still goes out on
 * the request its position earns and the trace is unchanged.
 *
 * A task chained behind the copy in a scatter gather list would be the tidier
 * shape and does not work here. A task carrying the scatter gather cycle type
 * waits for a peripheral request of its own, so it eats the request meant to
 * fetch the next entry and the burst stalls with the FIFO starved; one
 * carrying AUTO or BASIC does not wait, but ends the list, so only the first
 * entry is ever toggled. Both were measured on an LP_EM_CC2755P20.
 *
 * Both channels answer the same request, so the uDMA serves them one after
 * the other. The data channel is given high priority whenever the debug
 * channel is armed, so that it is always served first whichever of channel 2
 * and 4 the pin solver gave it; the toggle then only follows the
 * data, by one single word transfer. The attribute is written here on every
 * arm and not once at open, since the uDMA is reinitialized on wake from
 * standby, and taken off again in RCL_Dma_stop. */
static void rclDmaDebugArm(uint32_t numRequests)
{
    if (!RCL_DMA_DEBUG_PRESENT())
    {
        return;
    }

    if ((rclDmaDebugPinMask == 0U) || (numRequests == 0U) ||
        (numRequests > (uint32_t) UDMA_XFER_SIZE_MAX))
    {
        rclDmaDebugUnavailable++;
        return;
    }

    uDMASetChannelControl(RCL_dmaDebugControlTableEntry,
                          UDMA_SRC_INC_NONE | UDMA_DST_INC_NONE | UDMA_SIZE_32 | UDMA_ARB_1);
    uDMASetChannelTransfer(RCL_dmaDebugControlTableEntry,
                           UDMA_MODE_BASIC,
                           (void *) &rclDmaDebugPinMask,
                           (void *) (GPIO_BASE + GPIO_O_DOUTTGL31_0),
                           numRequests);
    uDMADisableChannelAttribute(RCL_dmaDebugChannelMask, UDMA_ATTR_USEBURST | UDMA_ATTR_HIGH_PRIORITY);
    uDMAEnableChannelAttribute(RCL_dmaChannelMask, UDMA_ATTR_HIGH_PRIORITY);
    UDMALPF3_channelEnable(RCL_dmaDebugChannelMask);
}

/*
 *  ======== RCL_Dma_setDebugPin ========
 */
void RCL_Dma_setDebugPin(uint_least8_t gpioIndex)
{
    /* The GPIO driver index is the DIO number on this device, and
     * DOUTTGL31_0 has one bit per DIO. Anything else, GPIO_INVALID_INDEX
     * included, leaves the pin alone. */
    rclDmaDebugPinMask = (gpioIndex < 32U) ? ((uint32_t) 1U << (uint32_t) gpioIndex) : 0U;
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
 *  ======== rclDmaQuiesce ========
 */
/* Take the trigger away and let the channel come to rest, so that the
 * transfer count read afterwards is complete.
 *
 * The selected FIFO condition is true again as soon as the radio has drained
 * the FIFO, so the request would stay asserted with no transfer left to serve,
 * and the uDMA waits for a request to fall before it arbitrates again. Leaving
 * it asserted parks the channel in that wait and the next command never gets
 * its data.
 *
 * With the request gone the channel finishes the arbitration it may be in and
 * only then writes its remaining count back to the control table. That takes
 * at most one arbitration of bus transfers, which is what the spin allows
 * for. */
static void rclDmaQuiesce(void)
{
    HWREG_WRITE_LRF(LRFDDBELL_BASE + LRFDDBELL_O_DMACFG) = 0U;
    HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG5) = LRFDPBE_FCFG5_DMAREQ_NONE |
                                                      LRFDPBE_FCFG5_DMASREQ_NONE;

    for (uint32_t i = 0U; i < RCL_DMA_SETTLE_SPINS; i++)
    {
        (void) uDMAGetChannelSize(RCL_dmaControlTableEntry);
    }
}

/*
 *  ======== rclDmaStartTx ========
 */
/* Transfer the posted data entries (length field, pad and packet) into the TX
 * FIFO through the FIFO data port. The port pushes as many bytes as the access
 * width, so a byte wide transfer pushes one byte per access, and the FIFO
 * advances TXFWP itself. No pointer write is needed, so RCL-367 does not
 * apply. The port is LRFDPBE.TXFHWR and not the LRFDTXF alias region, which
 * carries the RCL-367 defect described above; on transmit that defect drops
 * the FIFO command rather than the push, which this design would not even
 * notice, since OPCFG.TXFCMD is NONE and the PBE issues no FIFO command
 * during a TX burst. TXFHWR takes two bytes and advances TXFWP by two on
 * every access whatever the width of the bus access, so the transfer is
 * halfword wide; TXFBWR is the same port one byte at a time, which is why a
 * wide access to it moves a single byte.
 *
 * The channel is enabled before the request is raised. The FIFO was prepared
 * empty, so the request is due the moment the trigger is routed, and a
 * request raised while the channel was still disabled is not served once
 * the channel is enabled afterwards: the uDMA acts on the request as it
 * arrives. The trigger is therefore taken down and put up again here, after
 * the enable, which gives the channel a request of its own whether the
 * trigger was already routed by the arm or not. These are DBELL and PBE
 * configuration registers, not FIFO accesses, so this may run from any
 * context, including while the PBE is on the air. Returns whether the first
 * arbitration had completed by the time the spin below gave up. */
static bool rclDmaStartTx(const void *source, uint32_t numBytes)
{
    uDMASetChannelControl(RCL_dmaControlTableEntry,
                          UDMA_SRC_INC_16 | UDMA_DST_INC_NONE | UDMA_SIZE_16 | rclDmaTxArb);
    uDMASetChannelTransfer(RCL_dmaControlTableEntry,
                           UDMA_MODE_BASIC,
                           (void *) source,
                           (void *) (LRFDPBE_BASE + LRFDPBE_O_TXFHWR),
                           numBytes / 2U);
    uDMADisableChannelAttribute(RCL_dmaChannelMask, UDMA_ATTR_USEBURST);
    rclDmaState.txStarted = true;
    UDMALPF3_channelEnable(RCL_dmaChannelMask);

    /* One edge per entry the FIFO asks for */
    rclDmaDebugArm((rclDmaTxArbBytes != 0U) ? (numBytes / rclDmaTxArbBytes) : 0U);

    HWREG_WRITE_LRF(LRFDDBELL_BASE + LRFDDBELL_O_DMACFG) = 0U;
    rclDmaConfigureTrigger(LRFDPBE_FCFG5_DMAREQ_TXWRBTHR_MET | LRFDPBE_FCFG5_DMASREQ_NONE);

    /* The first arbitration follows at once, and the caller may not post the
     * operation until it has: the PBE pops the first entry's header as soon as
     * it is given the operation and an underflow there is unrecoverable. The
     * spin is bounded so that a channel which is not answering is reported
     * rather than waited for. */
    uint32_t spins = 0U;
    while ((uDMAGetChannelSize(RCL_dmaControlTableEntry) >= (numBytes / 2U)) &&
           (spins < RCL_DMA_TX_KICK_SPINS))
    {
        spins++;
    }
    return (spins < RCL_DMA_TX_KICK_SPINS);
}

/*
 *  ======== rclDmaPostTx ========
 */
/* Record what is to be transmitted and start the transfer if the FIFO is
 * already armed for it */
static int_fast16_t rclDmaPostTx(const void *source, uint32_t numBytes, uint32_t stride)
{
    int_fast16_t status = RCL_Dma_Status_Success;

    uintptr_t key = HwiP_disable();
    if (rclDmaState.txSource != NULL)
    {
        status = RCL_Dma_Status_Error_Busy;
    }
    else
    {
        rclDmaState.txSource = source;
        rclDmaState.txStride = stride;
        rclDmaState.txNumBytes = numBytes;
        if (rclDmaState.txArmed)
        {
            (void) rclDmaStartTx(source, numBytes);
        }
    }
    HwiP_restore(key);

    return status;
}

/*
 *  ======== rclDmaRxAllowSpace ========
 */
/* Hand the PBE the FIFO space the drain has freed.
 *
 * Reading through the FIFO data port moves RXFRP but not RXFSRP, so something
 * has to follow it. As long as the posted buffer can take a whole FIFO, the
 * hardware does: FCFG0.RXADEAL moves RXFSRP after RXFRP and nothing needs to be
 * written while a packet is on the air. That is what makes it safe, because
 * LRF_setRxFifoEffSz() parks the PBE FIFO command register on FSTAT for the
 * duration of its pointer write, and the PBE retries a FIFO command it issues
 * in that window until the register is restored: it stands still for as long
 * as the write takes, which in the middle of a packet is a stall of the
 * modulator or the demodulator. Called once per packet it eventually lands on
 * a command the PBE needed on time, and the command ends with an RX FIFO error
 * on a FIFO that is empty.
 *
 * Only a buffer with less room than the FIFO still needs the write, and then
 * auto deallocate has to come off so that the two do not both move RXFSRP. */
static void rclDmaRxAllowSpace(void)
{
    uint32_t fcfg0 = HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0);

    if ((fcfg0 & LRFDPBE_FCFG0_RXADEAL_M) == 0U)
    {
        HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0) = fcfg0 | LRFDPBE_FCFG0_RXADEAL_M;
    }
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

    if (RCL_DMA_DEBUG_PRESENT())
    {
        /* The same trigger, on the other channel it reaches */
        EVTSVTConfigureDma(RCL_dmaDebugChannelSubscriberId, EVTSVT_DMA_TRIG_LRFDTRG);
        UDMALPF3_disableAttribute(RCL_dmaDebugChannelMask, UDMA_ATTR_ALTSELECT);
    }

    rclDmaState.txSource = NULL;
    rclDmaState.txHeaderBytes = 0U;
    rclDmaState.txLastEntries = 0U;
    rclDmaState.rxBuffer = NULL;
    rclDmaState.txArmed = false;
    rclDmaState.txStarted = false;
    rclDmaState.rxArmed = false;
    rclDmaState.rxBurstArmed = false;
}

/*
 *  ======== RCL_Dma_close ========
 */
void RCL_Dma_close(void)
{
    HWREG_WRITE_LRF(LRFDDBELL_BASE + LRFDDBELL_O_DMACFG) = 0U;
    UDMALPF3_channelDisable(RCL_dmaChannelMask);
    if (RCL_DMA_DEBUG_PRESENT())
    {
        UDMALPF3_channelDisable(RCL_dmaDebugChannelMask);
    }
    Power_releaseDependency(PowerLPF3_PERIPH_DMA);
}

/*
 *  ======== RCL_Dma_putTxBurst ========
 */
int_fast16_t RCL_Dma_putTxBurst(RCL_Buffer_DataEntry *entries, uint32_t numEntries)
{
    int_fast16_t status = RCL_Dma_Status_Success;
    uint32_t stride = (entries != NULL) ? RCL_Buffer_DataEntry_paddedLen(entries->length) : 0U;
    uint32_t shift = 0U;

    while ((1U << shift) < stride)
    {
        shift++;
    }

    /* One arbitration has to be one entry, so the entry has to be a power of
     * two bytes and at least two of them, the port moving two bytes per
     * access. An entry of any other length would be split across two requests
     * and the FIFO would ask for the second half of a packet while the first
     * was already being modulated. */
    if ((entries == NULL) || (numEntries == 0U) || (stride < 2U) ||
        ((1U << shift) != stride) || ((numEntries * stride) > (2U * UDMA_XFER_SIZE_MAX)))
    {
        status = RCL_Dma_Status_Error_Param;
    }
    else
    {
        for (uint32_t i = 0U; i < numEntries; i++)
        {
            const RCL_Buffer_DataEntry *entry = (const RCL_Buffer_DataEntry *) ((const uint8_t *) entries + (i * stride));

            if (RCL_Buffer_DataEntry_paddedLen(entry->length) != stride)
            {
                status = RCL_Dma_Status_Error_Param;
            }
        }
    }

    if (status == RCL_Dma_Status_Success)
    {
        /* One arbitration of stride bytes is (stride / 2) port writes */
        rclDmaTxArb = (shift - 1U) << UDMA_ARB_S;
        rclDmaTxArbBytes = stride;
        rclDmaState.txHeaderBytes = sizeof(entries->length) + sizeof(entries->numPad) + entries->numPad;
        status = rclDmaPostTx(entries, numEntries * stride, stride);
    }

    return status;
}

/*
 *  ======== RCL_Dma_getTxEntriesTaken ========
 */
uint32_t RCL_Dma_getTxEntriesTaken(void)
{
    uint32_t taken = 0U;

    uintptr_t key = HwiP_disable();
    if (rclDmaState.txSource == NULL)
    {
        /* Nothing posted: whatever was posted last has been transmitted and
         * handed back, so every one of its entries counts as taken. Without
         * this a sample written between the end of the command and the next
         * post would go into an entry that has already been sent. */
        taken = rclDmaState.txLastEntries;
    }
    else if (rclDmaState.txStarted)
    {
        /* The uDMA writes the remaining count back at the end of every
         * arbitration, and an arbitration is one entry, so the count is
         * always a whole number of entries. It reads 0 once the transfer has
         * completed. */
        uint32_t remaining = uDMAGetChannelSize(RCL_dmaControlTableEntry) * 2U;

        if (remaining < rclDmaState.txNumBytes)
        {
            taken = (rclDmaState.txNumBytes - remaining) / rclDmaState.txStride;
        }
    }
    HwiP_restore(key);

    return taken;
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
        if (rclDmaState.rxArmed)
        {
            /* The armed transfer writes into the buffer that was posted when it
             * was armed, but RCL_Dma_finishRxBurst commits the bytes into
             * whichever buffer is posted at that point. Replacing the buffer
             * under an armed burst would therefore commit the received packets
             * into the wrong one. */
            status = RCL_Dma_Status_Error_Busy;
        }
        else
        {
            rxBuffer->state = RCL_BufferStateInUse;
            rclDmaState.rxBuffer = rxBuffer;
        }
        HwiP_restore(key);
    }

    return status;
}

/*
 *  ======== RCL_Dma_armTx ========
 */
int_fast16_t RCL_Dma_armTx(void)
{
    int_fast16_t status = RCL_Dma_Status_Error_Param;

    /* The FIFO frees space as the modulator consumes it, which is what paces
     * the refill. LRF_prepareTxFifo leaves auto deallocate off because the CPU
     * path retries the FIFO to repeat a packet; the DMA path does not. */
    HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0) =
        HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0) | LRFDPBE_FCFG0_TXADEAL_M;

    /* Nothing has been written since the FIFO was prepared, so this is its size */
    uint32_t fifoSize = HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_TXFWRITABLE);

    uintptr_t key = HwiP_disable();

    /* For a burst the request is raised only once the PBE is into an entry's
     * payload, not already when it has popped the entry's header. The PBE
     * pops the header of the first entry as soon as it is handed the
     * operation, at setup, and with the threshold one entry below the FIFO
     * size that pop would make the FIFO ask for the second entry then, long
     * before the first packet is on the air. Raising the threshold by the
     * header keeps the second entry out until the first payload byte is
     * popped, and every later entry likewise arrives as the packet before it
     * starts to modulate. One arbitration still takes the readable count to
     * at least an entry, above any of these thresholds, so the request falls. */
    uint32_t threshold = rclDmaFillThreshold(fifoSize, rclDmaTxArbBytes) + rclDmaState.txHeaderBytes;
    if (threshold > fifoSize)
    {
        threshold = fifoSize;
    }
    rclDmaConfigureTrigger(LRFDPBE_FCFG5_DMAREQ_TXWRBTHR_MET | LRFDPBE_FCFG5_DMASREQ_NONE);
    HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_TXFWBTHRS) = threshold;

    rclDmaState.txArmed = true;
    rclDmaState.txStarted = false;
    if (rclDmaState.txSource != NULL)
    {
        bool landed = rclDmaStartTx(rclDmaState.txSource, rclDmaState.txNumBytes);

        /* The first entry has to be in the FIFO before the caller hands the
         * operation to the PBE. Give a slow channel more time than the kick
         * spin does; one that still has not answered is reported, and the
         * caller must then not post the operation. */
        uint32_t spins = 0U;
        while (!landed && (spins < RCL_DMA_TX_ARM_SPINS))
        {
            landed = ((uDMAGetChannelSize(RCL_dmaControlTableEntry) * 2U) < rclDmaState.txNumBytes);
            spins++;
        }
        status = landed ? RCL_Dma_Status_Success : RCL_Dma_Status_Error_Fifo;
    }
    HwiP_restore(key);

    return status;
}

/*
 *  ======== RCL_Dma_armRxBurst ========
 */
int_fast16_t RCL_Dma_armRxBurst(uint32_t numEntries, uint32_t entryBytes)
{
    uint32_t numBytes = numEntries * entryBytes;
    uint32_t shift = 0U;

    /* Physical RX FIFO */
    uint32_t rxFifoSize = (HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG4) &
                           LRFDPBE_FCFG4_RXSIZE_M) >> LRFDPBE_FCFG4_RXSIZE_S << 2U;

    uintptr_t key = HwiP_disable();
    RCL_MultiBuffer *rxBuffer = rclDmaState.rxBuffer;
    HwiP_restore(key);

    uint32_t space = (rxBuffer != NULL) ? ((uint32_t) rxBuffer->length - rxBuffer->tailIndex) : 0U;

    while ((1U << shift) < entryBytes)
    {
        shift++;
    }

    /* One request is answered with one arbitration and one arbitration has to
     * be one entry, so the entry has to be a power of two bytes, and at least
     * two of them because the port moves two bytes per access.
     *
     * The burst is not bounded by the size of the FIFO: the port advances
     * RXFRP as it reads and FCFG0.RXADEAL hands the space straight back. The
     * posted buffer must take the whole burst and a whole FIFO, the latter
     * being what lets the space be returned by auto deallocate alone, so that
     * no FIFO pointer is written while the PBE is running. */
    if ((numEntries == 0U) || (entryBytes < 2U) || ((1U << shift) != entryBytes) ||
        (space < numBytes) || (space < rxFifoSize) ||
        ((numBytes / 2U) > (uint32_t) UDMA_XFER_SIZE_MAX))
    {
        return RCL_Dma_Status_Error_Param;
    }

    /* Give the PBE the FIFO space with one pointer write while it is not
     * running; auto deallocate keeps it open from then on. */
    LRF_setRxFifoEffSz(space);
    rclDmaRxAllowSpace();

    key = HwiP_disable();
    rclDmaState.rxArmed = true;
    rclDmaState.rxBurstArmed = true;
    rclDmaState.rxBurstDst = RCL_MultiBuffer_getNextWritableByte(rxBuffer);
    rclDmaState.rxBurstEntry = entryBytes;
    rclDmaState.rxBurstStreamBytes = numBytes;
    HwiP_restore(key);

    /* Arm the transfer for the whole burst and route the trigger, so that each
     * entry is taken out of the FIFO as the PBE commits it. One arbitration of
     * entryBytes is (entryBytes / 2) port reads. */
    uDMASetChannelControl(RCL_dmaControlTableEntry,
                          UDMA_SRC_INC_NONE | UDMA_DST_INC_16 | UDMA_SIZE_16 |
                          ((shift - 1U) << UDMA_ARB_S));
    uDMASetChannelTransfer(RCL_dmaControlTableEntry,
                           UDMA_MODE_BASIC,
                           (void *) (LRFDPBE_BASE + LRFDPBE_O_RXFHRD),
                           rclDmaState.rxBurstDst,
                           numBytes / 2U);
    uDMADisableChannelAttribute(RCL_dmaChannelMask, UDMA_ATTR_USEBURST);
    UDMALPF3_channelEnable(RCL_dmaChannelMask);

    /* One edge per committed entry; the receive trigger is a pulse */
    rclDmaDebugArm(numEntries);

    rclDmaConfigureTrigger(LRFDPBE_FCFG5_DMAREQ_RXFIFO_COMMIT | LRFDPBE_FCFG5_DMASREQ_NONE);

    return RCL_Dma_Status_Success;
}

/*
 *  ======== RCL_Dma_finishRxBurst ========
 */
uint32_t RCL_Dma_finishRxBurst(const uint8_t **firstEntry)
{
    uint32_t numBytes = 0U;
    RCL_MultiBuffer *rxBuffer = rclDmaState.rxBuffer;

    *firstEntry = NULL;

    if (rclDmaState.rxBurstArmed && (rxBuffer != NULL))
    {
        /* Take the trigger away and let the channel come to rest, so that the
         * count read below is the whole of what the transfer moved. */
        rclDmaQuiesce();
        UDMALPF3_channelDisable(RCL_dmaChannelMask);

        /* The port moves two bytes per access, so the count is halfwords */
        uint32_t armed = rclDmaState.rxBurstStreamBytes / 2U;
        uint32_t left = uDMAGetChannelSize(RCL_dmaControlTableEntry);

        if (left > armed)
        {
            left = armed;
        }
        numBytes = (armed - left) * 2U;

        if (numBytes > 0U)
        {
            RCL_MultiBuffer_commitBytes(rxBuffer, numBytes);
        }
        rclDmaState.rxBurstStreamBytes = 0U;

        /* A burst that ended early leaves the entries the transfer did not
         * reach in the FIFO, in order. The operation has ended, so the PBE
         * issues no FIFO command and the read pointer may be moved: that is
         * what LRF_readRxFifoWords does, out of the BUFRAM window and with
         * the protected pointer write. */
        uint32_t readable = HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_RXFREADABLE);

        if ((readable % rclDmaState.rxBurstEntry) != 0U)
        {
            rclDmaRxBurstStray++;
            rclDmaRxBurstStrayBytes = readable % rclDmaState.rxBurstEntry;
        }

        uint32_t room = (uint32_t) rxBuffer->length - rxBuffer->tailIndex;
        if (readable > room)
        {
            readable = room;
        }
        readable &= ~0x0003U;
        if (readable > 0U)
        {
            LRF_readRxFifoWords((uint32_t *) RCL_MultiBuffer_getNextWritableByte(rxBuffer),
                                readable / sizeof(uint32_t));
            RCL_MultiBuffer_commitBytes(rxBuffer, readable);
            numBytes += readable;
        }

        if (numBytes > 0U)
        {
            *firstEntry = rclDmaState.rxBurstDst;
        }

        rclDmaState.rxBurstArmed = false;
    }

    return numBytes;
}

/*
 *  ======== RCL_Dma_stop ========
 */
void RCL_Dma_stop(void)
{
    /* Takes the trigger away, see rclDmaQuiesce */
    rclDmaQuiesce();

    uint32_t fstat = HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_FSTAT);

    if (rclDmaState.txArmed)
    {
        if ((fstat & LRFDPBE_FSTAT_TXUNFL_M) != 0U)
        {
            rclDmaTxUnderrun++;
        }
        if ((fstat & LRFDPBE_FSTAT_TXOVFL_M) != 0U)
        {
            rclDmaTxOverflow++;
        }
    }

    if (rclDmaState.rxArmed)
    {
        if ((fstat & LRFDPBE_FSTAT_RXOVFL_M) != 0U)
        {
            rclDmaRxOverflow++;
        }
    }

    uintptr_t key = HwiP_disable();
    UDMALPF3_channelDisable(RCL_dmaChannelMask);
    if (RCL_DMA_DEBUG_PRESENT())
    {
        UDMALPF3_channelDisable(RCL_dmaDebugChannelMask);
        uDMADisableChannelAttribute(RCL_dmaChannelMask, UDMA_ATTR_HIGH_PRIORITY);
    }
    rclDmaState.txArmed = false;
    rclDmaState.txStarted = false;
    rclDmaState.rxArmed = false;
    rclDmaState.rxBurstArmed = false;
    rclDmaState.rxBurstStreamBytes = 0U;
    if (rclDmaState.txSource != NULL)
    {
        rclDmaState.txLastEntries = rclDmaState.txNumBytes / rclDmaState.txStride;
    }
    rclDmaState.txSource = NULL;
    HwiP_restore(key);
}

#endif /* DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX */
