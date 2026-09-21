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
/*
 *  ======== generic.c ========
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <ti/log/Log.h>

#include <ti/drivers/rcl/RCL_Command.h>
#include <ti/drivers/rcl/RCL_Buffer.h>
#include <ti/drivers/rcl/RCL_Scheduler.h>
#include <ti/drivers/rcl/RCL_Profiling.h>
#include <ti/drivers/rcl/RCL_Feature.h>

#include <ti/drivers/rcl/LRF.h>
#include <ti/drivers/rcl/hal/RCL_Hal.h>
#include <ti/drivers/rcl/commands/generic.h>
#include <ti/drivers/dpl/HwiP.h>

#include <ti/devices/DeviceFamily.h>

/* The generic burst commands run their data path on the LRF DMA, and exist
 * only on the device family that has it. They program the generic PBE RAM
 * directly.
 */
#if (DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX)
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/pbe_generic_ram_regs.h)
#include DeviceFamily_constructPath(inc/hw_lrfdpbe.h)
#include DeviceFamily_constructPath(inc/hw_lrfdrfe.h)
#include DeviceFamily_constructPath(inc/hw_lrfdrfe32.h)
#include DeviceFamily_constructPath(inc/hw_lrfdmdm.h)
#include DeviceFamily_constructPath(inc/hw_lrfdmdm32.h)
#include DeviceFamily_constructPath(inc/rfe_common_ram_regs.h)
#include <ti/drivers/rcl/RCL_Dma.h>

/* Packet count at which the PBE ends a burst of its own accord, one word per
 * direction, and the transmitted-packet count at which it writes NTX back to
 * zero and raises its interrupt 9. Zero disables each. The PBE zeroes the two
 * targets as it tears an operation down, so they are written on every burst;
 * it never touches NTXIRQ, so every transmit command writes that one at
 * setup, the stream with what it needs and the others with zero.
 *
 * They sit past the end of the generated PBE_GENERIC_RAM map and are spelled
 * out here rather than taken from pbe_generic_ram_regs.h, which does not carry
 * them; the offsets are the PBE port numbers 0xdf, 0xe0 and 0xe1 as BUFRAM
 * byte offsets, (port - 0x80) * 2. */
#define PBE_GENERIC_RAM_O_NTXTARGET 0x000000BEU
#define PBE_GENERIC_RAM_O_NRXTARGET 0x000000C0U
#define PBE_GENERIC_RAM_O_NTXIRQ    0x000000C2U

/* The PBE's interrupt 9 reaches the CPU as doorbell bit 9, which the RCL
 * names after what the BLE image raises it for. The generic image leaves it
 * unused and its NTXIRQ patch raises it at every hop of a transmit stream;
 * this is the bit's name for that. */
#define LRF_EventStreamHop LRF_EventTxCtrl

/*
 *  ======== rclGenericTxResetCountStops ========
 *
 *  Zero the two words that end or interrupt a transmit operation on its
 *  packet count, from every transmit command's setup. Both outlive the
 *  command that wrote them: the PBE zeroes NTXTARGET only as it tears an
 *  operation down and NTXIRQ never, and until some command has written them
 *  they hold whatever the RAM powered up with.
 */
static void rclGenericTxResetCountStops(void)
{
    HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NTXTARGET) = 0U;
    HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NTXIRQ) = 0U;
}
#else
/* The words are the patched CC27XX generic image's; nothing to reset elsewhere */
static inline void rclGenericTxResetCountStops(void)
{
}
#endif

struct
{
    struct {
        uint16_t                txFifoSize;
        uint16_t                rxFifoSize;
        RCL_CommandStatus       endStatus;
        bool                    activeUpdate;
        bool                    powerStandbyConstraintSet;
        bool                    powerSwtcxoConstraintSet;
        RCL_MultiBuffer         *curBuffer;
    } common;
    union {
        struct {
            bool                gracefulStopObserved;
            bool                stopFs;
            uint32_t            txCount;
            uint32_t            period;
        } tx;
        struct {
            LRF_ModulationCtrl  modulationCtrl;
        } txTest;
        struct {
            uint32_t            longOkCount;
            uint32_t            longNokCount;
            LRF_SyncSearchCtrl  syncSearchCtrl;
        } rx;
        struct {
            uint32_t            longTxCount;
            uint32_t            longOkCount;
            uint32_t            longNokCount;
            uint32_t            longRxIgnoredCount;
            uint32_t            longRxAddrMismatchCount;
            uint32_t            longRxBufFullCount;
            LRF_SyncSearchCtrl  syncSearchCtrl;
        } nesb;
        struct {
            RCL_StopType        stopType;
            bool                calibrating;    /* TX: the synthesizer calibration operation is in flight */
            uint16_t            fifoCfg;        /* RX: FIFOCFG as the settings left it, restored at the end */
            uint16_t            extraBytes;     /* RX: EXTRABYTES likewise */
            uint8_t             numHops;        /* Channels in the hop table; 0: no hopping */
            uint8_t             packetsPerHop;
            uint8_t             channel;        /* The row the radio is on */
            uint16_t            target;         /* RX: the count the running operation ends on; 0: none armed */
            uint32_t            hops;           /* Hops made, the missed ones included */
            uint32_t            hopsMissed;     /* TX: hops whose row was not written, the RFE not being idle */
            uint32_t            rxOk;           /* RX: the counts of the operations that have ended */
            uint32_t            rxNok;
            uint32_t            *rows;          /* The command's row storage */
        } stream;
    };
} genericHandlerState;


static uint32_t RCL_Handler_Generic_prepareSynth(void);
static void RCL_Handler_Generic_setSynthPowerState(bool fsOff);
static void RCL_Handler_Generic_updateRxCurBufferAndFifo(List_List *rxBuffers);
static RCL_CommandStatus RCL_Handler_Generic_mapLrfErrorStatusToRclStatus(void);
#if (DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX)
static uint32_t RCL_Handler_Generic_findRxStatusPositionFromEnd(void);
#endif
static uint32_t RCL_Handler_Generic_updateTxBuffers(List_List *txBuffers, uint32_t maxBuffers);
static void RCL_Handler_Generic_updateRxStats(RCL_StatsGeneric *stats, uint32_t startTime);
static void RCL_Handler_Generic_updateLongStats(void);
static bool RCL_Handler_Generic_initRxStats(RCL_StatsGeneric *stats, uint32_t startTime);
static void RCL_Handler_Nesb_updateHeader(List_List *txBuffers, uint8_t autoRetransmitMode,
                                          uint8_t hdrConf, uint8_t seqNumber);
static void RCL_Handler_Nesb_updateStats(RCL_StatsNesb *stats, uint32_t startTime);
static void RCL_Handler_Nesb_updateLongStats(void);
static bool RCL_Handler_Nesb_initStats(RCL_StatsNesb *stats, uint32_t startTime);

/*
 *  ======== RCL_Handler_Generic_Fs ========
 */
RCL_Events RCL_Handler_Generic_Fs(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_CmdGenericFs *fsCmd = (RCL_CmdGenericFs *) cmd;
    RCL_Events rclEvents = {.value = 0U};

    if (rclEventsIn.setup != 0U)
    {
        uint32_t earliestStartTime;

        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();

        /* Program frequency word */
        LRF_programFrequency(fsCmd->rfFrequency, fsCmd->fsType == RCL_FsType_Tx);

        /* Enable radio */
        LRF_enable();

        /* Mark as active */
        cmd->status = RCL_CommandStatus_Active;
        /* Default end status */
        genericHandlerState.common.endStatus = RCL_CommandStatus_Finished;
        /* Configure LRF for running operation Fs */
        LRF_Interface_Generic_configOpFs();

        RCL_CommandStatus startTimeStatus = RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime);
        if (startTimeStatus >= RCL_CommandStatus_Finished)
        {
            cmd->status = startTimeStatus;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Enable interrupts */
            LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);

            /* Post cmd */
            Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Generic_Fs: Starting Frequency Synthesizer");
            LRF_waitForTopsmReady();
            LRF_Interface_Generic_sendOpFs();
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (rclEventsIn.timerStart != 0U)
        {
            rclEvents.cmdStarted = 1U;
        }
        if (lrfEvents.opDone != 0U)
        {
            cmd->status = genericHandlerState.common.endStatus;

            RCL_Handler_Generic_setSynthPowerState(false);

            /* Set additional power constraints if necessary */
            if(!genericHandlerState.common.powerStandbyConstraintSet)
            {
                genericHandlerState.common.powerStandbyConstraintSet = true;
                RCL_Hal_powerSetStandbyConstraint();
            }
            rclEvents.lastCmdDone = 1U;
        }
        else if (lrfEvents.opError != 0U)
        {
            RCL_CommandStatus endStatus = genericHandlerState.common.endStatus;
            if (endStatus == RCL_CommandStatus_Finished)
            {
                cmd->status = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
            }
            else
            {
                cmd->status = endStatus;
            }
            RCL_Handler_Generic_setSynthPowerState(true);
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Other events need to be handled unconditionally */
        }
    }
    if (rclEvents.lastCmdDone != 0U)
    {
        LRF_disable();
    }
    return rclEvents;
}

/*
 *  ======== RCL_Handler_Generic_FsOff ========
 */
RCL_Events RCL_Handler_Generic_FsOff(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_Events rclEvents = {.value = 0U};

    if (rclEventsIn.setup != 0U)
    {
        /* Enable radio */
        LRF_enable();

        /* Mark as active */
        cmd->status = RCL_CommandStatus_Active;

        RCL_CommandStatus startTimeStatus = RCL_Scheduler_setCmdStopTimeNoStartTrigger(cmd);
        if (startTimeStatus >= RCL_CommandStatus_Finished)
        {
            cmd->status = startTimeStatus;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Configure LRF for running operation FsOff */
            LRF_Interface_Generic_configOpFsOff();

            /* Enable interrupts */
            LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);

            /* Post cmd */
            Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Generic_FsOff: Turning off Frequency Synthesizer");
            LRF_waitForTopsmReady();
            LRF_Interface_Generic_sendOpFsOff();
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (lrfEvents.opDone != 0U)
        {
            cmd->status = RCL_CommandStatus_Finished;
            RCL_Handler_Generic_setSynthPowerState(true);
            rclEvents.lastCmdDone = 1U;
        }
        else if (lrfEvents.opError != 0U)
        {
            cmd->status = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Other events need to be handled unconditionally */
        }
    }
    if (rclEvents.lastCmdDone != 0U)
    {
        LRF_disable();
    }
    return rclEvents;
}

/*
 *  ======== RCL_Handler_Generic_Tx ========
 */
RCL_Events RCL_Handler_Generic_Tx(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_CmdGenericTx *txCmd = (RCL_CmdGenericTx *) cmd;
    RCL_Events rclEvents = {.value = 0U};

    if (rclEventsIn.setup != 0U)
    {
        uint32_t earliestStartTime;

        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();

        if ((txCmd->rfFrequency == 0U) && (LRF_Interface_Generic_isFreqSynthLocked() == false))
        {
            /* Synth not to be programmed, but not already locked */
            cmd->status = RCL_CommandStatus_Error_Synth;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Program the sync word provided in the RCL_CmdGenericTx radio command to the LRF */
            LRF_Interface_Generic_programSyncWordA(txCmd->syncWord);

            /* Configure LRF for running operation generix tx */
            LRF_Interface_Generic_configOpTx(txCmd->config.fsOff, txCmd->rfFrequency);

            /* Mark as active */
            cmd->status = RCL_CommandStatus_Active;
            /* Default end status */
            genericHandlerState.common.endStatus = RCL_CommandStatus_Finished;

            /* Program frequency word */
            if (txCmd->rfFrequency != 0U)
            {
                LRF_programFrequency(txCmd->rfFrequency, true);
            }
            if (LRF_programTxPower(txCmd->txPower, txCmd->rfFrequency) != TxPowerResult_Ok)
            {
                cmd->status = RCL_CommandStatus_Error_Param;
                rclEvents.lastCmdDone = 1U;
            }

            /* Enable radio */
            LRF_enable();

            /* Initialize RF FIFO */
            genericHandlerState.common.txFifoSize = (uint16_t) LRF_prepareTxFifo();
            rclGenericTxResetCountStops();

            /* Enter payload */
            uint32_t nBuffer = RCL_Handler_Generic_updateTxBuffers(&txCmd->txBuffers, 1U);
            if (nBuffer == 0U)
            {
                cmd->status = RCL_CommandStatus_Error_MissingTxBuffer;
                rclEvents.lastCmdDone = 1U;
            }
            else
            {
                RCL_CommandStatus startTimeStatus = RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime);
                if (startTimeStatus >= RCL_CommandStatus_Finished)
                {
                    cmd->status = startTimeStatus;
                    rclEvents.lastCmdDone = 1U;
                }
                else
                {
                    /* Enable interrupts */
                    LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);

                    /* Post cmd */
                    Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Generic_Tx: Starting TX");
                    LRF_waitForTopsmReady();
                    RCL_Profiling_eventHook(RCL_ProfilingEvent_PreprocStop);
                    LRF_Interface_Generic_sendOpTx();
                }
            }
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (rclEventsIn.timerStart != 0U)
        {
            rclEvents.cmdStarted = 1U;
        }
        if (lrfEvents.opDone != 0U)
        {
            cmd->status = genericHandlerState.common.endStatus;
            rclEvents.lastCmdDone = 1U;
            /* Pop transmitted packet */
            RCL_Buffer_TxBuffer *txBuffer;
            txBuffer = RCL_TxBuffer_get(&txCmd->txBuffers);
            if (txBuffer != NULL)
            {
                txBuffer->state = RCL_BufferStateFinished;
            }
            RCL_Profiling_eventHook(RCL_ProfilingEvent_PostprocStart);
        }
        else if (lrfEvents.opError != 0U)
        {
            RCL_CommandStatus endStatus = genericHandlerState.common.endStatus;
            if (endStatus == RCL_CommandStatus_Finished)
            {
                cmd->status = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
            }
            else
            {
                cmd->status = endStatus;
            }
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Other events need to be handled unconditionally */
        }
    }

    if (rclEvents.lastCmdDone != 0U)
    {
        LRF_disable();
        RCL_Handler_Generic_setSynthPowerState((bool) txCmd->config.fsOff);
    }
    return rclEvents;
}

RCL_Events RCL_Handler_Generic_TxRepeat(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_CmdGenericTxRepeat *txCmd = (RCL_CmdGenericTxRepeat *) cmd;
    RCL_Events rclEvents = {.value = 0U};
    bool runTx = false;

    if (rclEventsIn.setup != 0U)
    {
        uint32_t earliestStartTime;

        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();

        if ((txCmd->rfFrequency == 0U) && (LRF_Interface_Generic_isFreqSynthLocked() == false))
        {
            /* Synth not to be programmed, but not already locked */
            cmd->status = RCL_CommandStatus_Error_Synth;
            rclEvents.lastCmdDone = 1U;
        }
        else if ((txCmd->rfFrequency == 0U) && (txCmd->config.fsRecal != 0U))
        {
            /* Synth not to be programmed, recalibration for each packet requested */
            cmd->status = RCL_CommandStatus_Error_Param;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {

            /* Program the sync word provided in the RCL_CmdGenericTx radio command to the LRF */
            LRF_Interface_Generic_programSyncWordA(txCmd->syncWord);

            /* Configure LRF for running operation generic tx repeat */
            LRF_Interface_Generic_configOpTxRepeat(txCmd->rfFrequency, txCmd->config.fsRecal);

            /* Mark as active */
            cmd->status = RCL_CommandStatus_Active;
            /* Default end status */
            genericHandlerState.common.endStatus = RCL_CommandStatus_Finished;
            genericHandlerState.tx.stopFs = false;
            genericHandlerState.tx.txCount = 0U;

            /* Program frequency word */
            if (txCmd->rfFrequency != 0U)
            {
                LRF_programFrequency(txCmd->rfFrequency, true);
            }
            if (LRF_programTxPower(txCmd->txPower, txCmd->rfFrequency) != TxPowerResult_Ok)
            {
                cmd->status = RCL_CommandStatus_Error_Param;
                rclEvents.lastCmdDone = 1U;
            }

            /* Enable radio */
            LRF_enable();

            /* Initialize RF FIFO */
            genericHandlerState.common.txFifoSize = (uint16_t) LRF_prepareTxFifo();
            rclGenericTxResetCountStops();

            /* Enter payload */
            if (txCmd->txEntry == NULL)
            {
                cmd->status = RCL_CommandStatus_Error_MissingTxBuffer;
                rclEvents.lastCmdDone = 1U;
            }
            else
            {
                uint32_t length = txCmd->txEntry->length;
                /* Number of words including length field and end padding */
                uint32_t wordLength = RCL_Buffer_DataEntry_paddedLen(length) / 4U;
                if (wordLength > LRF_getTxFifoWritable() / 4U)
                {
                    /* Packet will not fit */
                    /* TODO: See RCL-348 */
                    cmd->status = RCL_CommandStatus_Error_Param;
                    rclEvents.lastCmdDone = 1U;
                }
                else
                {
                    LRF_writeTxFifoWords((uint32_t *) txCmd->txEntry, wordLength);

                    RCL_CommandStatus startTimeStatus = RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime);
                    if (startTimeStatus >= RCL_CommandStatus_Finished)
                    {
                        cmd->status = startTimeStatus;
                        rclEvents.lastCmdDone = 1U;
                    }
                    else
                    {
                        genericHandlerState.tx.period = txCmd->timePeriod;

                        runTx = true; /* Go on */
                        genericHandlerState.tx.gracefulStopObserved = false;
                        if (rclSchedulerState.gracefulStopInfo.cmdStopEnabled || rclSchedulerState.gracefulStopInfo.schedStopEnabled)
                        {
                            /* Enable interrupt to service graceful stop */
                            RCL_Hal_enableGracefulStopTimeIrq();
                        }
                    }
                }
            }
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (rclEventsIn.timerStart != 0U)
        {
            rclEvents.cmdStarted = 1U;
        }
        if (lrfEvents.systim1 != 0U)
        {
            genericHandlerState.tx.gracefulStopObserved = true;
        }
        if (lrfEvents.opDone != 0U)
        {
            if (genericHandlerState.tx.stopFs)
            {
                runTx = false;
            }
            else
            {
                /* Retry TX FIFO */
                LRF_retryTxFifo();

                if (txCmd->numPackets == 0U || genericHandlerState.tx.txCount < txCmd->numPackets)
                {
                    /* Configure LRF for next tx operation if there are more packets to transmit */
                    LRF_Interface_Generic_configNextOpTx();

                    runTx = true;
                    if (rclEventsIn.hardStop != 0U)
                    {
                        genericHandlerState.common.endStatus = RCL_Scheduler_findStopStatus(RCL_StopType_Hard);
                        runTx = false;
                    }
                    else if (LRF_Interface_isCmdEndCauseEopStop() == true ||
                             genericHandlerState.tx.gracefulStopObserved ||
                             rclEventsIn.gracefulStop != 0U)
                    {
                        genericHandlerState.common.endStatus = RCL_Scheduler_findStopStatus(RCL_StopType_Graceful);
                        runTx = false;
                    }
                    else
                    {
                        RCL_CommandStatus startTimeStatus;
                        if (genericHandlerState.tx.period != 0U)
                        {
                            startTimeStatus = RCL_Scheduler_setNewStartRelTime(genericHandlerState.tx.period);
                        }
                        else
                        {
                            startTimeStatus = RCL_Scheduler_setNewStartNow();
                        }
                        if (startTimeStatus >= RCL_CommandStatus_Finished)
                        {
                            genericHandlerState.common.endStatus = startTimeStatus;
                            runTx = false;
                        }
                    }
                }
                else
                {
                    if (LRF_Interface_isCmdEndCauseEopStop() == true)
                    {
                        genericHandlerState.common.endStatus = RCL_Scheduler_findStopStatus(RCL_StopType_Graceful);
                    }
                    runTx = false;
                }
            }
            if (!runTx && cmd->status == RCL_CommandStatus_Active)
            {
                if (!genericHandlerState.tx.stopFs && txCmd->config.fsRecal == 0U && txCmd->config.fsOff != 0U)
                {
                    /* Send stop FS */
                    LRF_waitForTopsmReady();
                    LRF_Interface_Generic_sendOpFsOff();
                    genericHandlerState.tx.stopFs = true;
                }
                else {
                    cmd->status = genericHandlerState.common.endStatus;
                    rclEvents.lastCmdDone = 1U;
                }
            }
        }
        else if (lrfEvents.opError != 0U)
        {
            if (genericHandlerState.common.endStatus == RCL_CommandStatus_Finished)
            {
                genericHandlerState.common.endStatus = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
            }

            if (!genericHandlerState.tx.stopFs && txCmd->config.fsRecal == 0U && txCmd->config.fsOff != 0U &&
                (LRF_Interface_Generic_isFreqSynthLocked() == true))
            {
                /* Synth was turned on, but should be off. Send stop FS */
                LRF_waitForTopsmReady();
                LRF_Interface_Generic_sendOpFsOff();
                genericHandlerState.tx.stopFs = true;
            }
            else
            {
                cmd->status = genericHandlerState.common.endStatus;
                rclEvents.lastCmdDone = 1U;
            }
        }
        else
        {
            /* Other events need to be handled unconditionally */
        }

        if (runTx)
        {
            uint32_t txCount = genericHandlerState.tx.txCount;
            if (txCount != 0U && txCmd->config.fsRecal == 0U)
            {
                /* Frequency programming only for the first packet */
                LRF_Interface_Generic_skipFreqProgramming();
            }
            txCount++;
            if (txCount != 0U)
            {
                /* Avoid wraparound */
                genericHandlerState.tx.txCount = txCount;
            }
            /* Enable interrupts */
            LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);

            /* Post cmd */
            Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Generic_TxRepeat: Starting TX");
            LRF_waitForTopsmReady();
            LRF_Interface_Generic_sendOpTx();
        }
    }
    if (rclEvents.lastCmdDone != 0U)
    {
        LRF_disable();
        RCL_Handler_Generic_setSynthPowerState((bool) txCmd->config.fsOff);
    }

    return rclEvents;
}

/*
 *  ======== RCL_Handler_Generic_TxTest ========
 */
RCL_Events RCL_Handler_Generic_TxTest(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_CmdGenericTxTest *txCmd = (RCL_CmdGenericTxTest *) cmd;
    RCL_Events rclEvents = { .value = 0U };

    if (rclEventsIn.setup != 0U)
    {
        uint32_t earliestStartTime;

        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();
        /* Reset whitening control state to avoid carryover from previous operations */
        genericHandlerState.txTest.modulationCtrl = (LRF_ModulationCtrl) { 0U };
        if ((txCmd->rfFrequency == 0U) && (LRF_Interface_Generic_isFreqSynthLocked() == false))
        {
            /* Synth not to be programmed, but not already locked */
            cmd->status = RCL_CommandStatus_Error_Synth;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Cache tx word as it will be used multiple times */
            uint32_t txWord = txCmd->config.txWord;
            /* Configure the LRF for running operation Tx Test */
            LRF_Interface_Generic_configOpTxTest(txCmd->config.fsOff, txCmd->rfFrequency, txWord);

            /* LRF modulation mode is controlled differently based on the configuration per RCL_CmdGenericTxTest command */
            LRF_Interface_Generic_setModulationMode(txCmd->config.sendCw, txCmd->config.whitenMode, &(genericHandlerState.txTest.modulationCtrl));

            /* Program a sync word only when the radio is not sending a carrier wave */
            if (txCmd->config.sendCw == 0U)
            {
                if (txCmd->config.whitenMode == RCL_CMD_GENERIC_WH_MODE_NONE)
                {
                    /* Use pattern as sync word */
                    LRF_Interface_Generic_programSyncWordA(txWord | (txWord << 16U));
                }
                else
                {
                    /* Use pseudo-random sync word (not necessarily matching selected PRBS) */
                    LRF_Interface_programSyncWordA(LRF_INTERFACE_GENERIC_PRBS_SYNC);
                }
            }
            else
            {
                /* No sync word is needed for a carrier wave */
            }

            /* Mark as active */
            cmd->status = RCL_CommandStatus_Active;
            /* Default end status */
            genericHandlerState.common.endStatus = RCL_CommandStatus_Finished;

            if (LRF_programTxPower(txCmd->txPower, txCmd->rfFrequency) != TxPowerResult_Ok)
            {
                cmd->status = RCL_CommandStatus_Error_Param;
                rclEvents.lastCmdDone = 1U;
            }

            /* Enable radio */
            LRF_enable();
            rclGenericTxResetCountStops();

            RCL_CommandStatus startTimeStatus = RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime);
            if (startTimeStatus >= RCL_CommandStatus_Finished)
            {
                cmd->status = startTimeStatus;
                rclEvents.lastCmdDone = 1U;
            }
            else
            {
                if (txCmd->rfFrequency != 0U)
                {
                    /* Program frequency word */
                    LRF_programFrequency(txCmd->rfFrequency, true);
                }

                /* Enable interrupts */
                LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);

                /* Post cmd */
                Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Generic_TxTest: Starting infinite TX");
                LRF_waitForTopsmReady();
                LRF_Interface_Generic_sendOpTx();
            }
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (rclEventsIn.timerStart != 0U)
        {
            rclEvents.cmdStarted = 1U;
        }
        if (lrfEvents.opDone != 0U)
        {
            cmd->status = genericHandlerState.common.endStatus;
            rclEvents.lastCmdDone = 1U;
        }
        else if (lrfEvents.opError != 0U)
        {
            RCL_CommandStatus endStatus = genericHandlerState.common.endStatus;
            if (endStatus == RCL_CommandStatus_Finished)
            {
                cmd->status = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
            }
            else
            {
                cmd->status = endStatus;
            }
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Other events need to be handled unconditionally */
        }
    }

    if (rclEvents.lastCmdDone != 0U)
    {
        LRF_disable();
        RCL_Handler_Generic_setSynthPowerState((bool) txCmd->config.fsOff);
        LRF_Interface_Generic_restoreModulationMode(&(genericHandlerState.txTest.modulationCtrl));
    }

    return rclEvents;
}

/*
 *  ======== RCL_Handler_Generic_Rx  ========
 */
RCL_Events RCL_Handler_Generic_Rx(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_CmdGenericRx *rxCmd = (RCL_CmdGenericRx *) cmd;
    RCL_Events rclEvents = RCL_EventNone;
    if (rclEventsIn.setup != 0U)
    {
        uint32_t earliestStartTime;

        /* Reset sync search control state to avoid carryover from previous operations */
        genericHandlerState.rx.syncSearchCtrl = (LRF_SyncSearchCtrl) { 0U };

        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();

        if ((rxCmd->rfFrequency == 0U) && (LRF_Interface_Generic_isFreqSynthLocked() == false))
        {
            /* Synth not to be programmed, but not already locked */
            cmd->status = RCL_CommandStatus_Error_Synth;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Program sync words provided by the radio command to the LRF */
            LRF_Interface_Generic_programSyncWordA(rxCmd->syncWordA);
            LRF_Interface_Generic_programSyncWordB(rxCmd->syncWordB);

            /* Configure LRF for running operation generic rx */
            LRF_Interface_Generic_configOpRx(rxCmd->config.fsOff, rxCmd->rfFrequency, rxCmd->config.repeated, rxCmd->maxPktLen);

            /* Disable sync search for syncwordA and syncwordB if requested by the radio configuration */
            bool disableSyncA = (rxCmd->config.disableSyncA != 0U);
            bool disableSyncB = (rxCmd->config.disableSyncB != 0U);
            /* To avoid the overhead of calling functions by checking the parameters first */
            if (disableSyncA == true || disableSyncB == true)
            {
                LRF_Interface_Generic_disableSyncSearch(disableSyncA, disableSyncB, &(genericHandlerState.rx.syncSearchCtrl));
            }

            /* Mark as active */
            cmd->status = RCL_CommandStatus_Active;
            /* Default end status */
            genericHandlerState.common.endStatus = RCL_CommandStatus_Finished;

            /* Program frequency word */
            if (rxCmd->rfFrequency != 0U)
            {
                LRF_programFrequency(rxCmd->rfFrequency, false);
            }

            /* Enable radio */
            LRF_enable();

            RCL_CommandStatus startTimeStatus = RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime);
            if (startTimeStatus >= RCL_CommandStatus_Finished)
            {
                cmd->status = startTimeStatus;
                rclEvents.lastCmdDone = 1U;
            }
            else {
                genericHandlerState.common.activeUpdate = RCL_Handler_Generic_initRxStats(rxCmd->stats,
                                                                                          rclSchedulerState.actualStartTime);
                /* Set up sync found capture */
                RCL_Hal_setupSyncFoundCap();
                /* Initialize RF FIFOs */
                genericHandlerState.common.rxFifoSize = (uint16_t) LRF_prepareRxFifo();
                genericHandlerState.common.curBuffer = NULL;
                if (rxCmd->config.discardRxPackets == 0U)
                {
                    RCL_Handler_Generic_updateRxCurBufferAndFifo(&rxCmd->rxBuffers);
                }
                else
                {
                    /* Set FIFO size to maximum */
                    LRF_setRxFifoEffSz(genericHandlerState.common.rxFifoSize);
                }

                /* Enable interrupts */
                uint16_t fifoCfg = LRF_Interface_Generic_getFifoCfg();
                LRF_enableHwInterrupt(LRF_Interface_Generic_maskEventsByFifoConf(LRF_EventOpDone.value | LRF_EventOpError.value |
                                                                                 LRF_EventRxOk.value | LRF_EventRxNok.value |
                                                                                 LRF_EventRxBufFull.value,
                                                                                 fifoCfg,
                                                                                 genericHandlerState.common.activeUpdate));
                /* Post cmd */
                Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Generic_Rx: Starting Rx");
                LRF_waitForTopsmReady();
                RCL_Profiling_eventHook(RCL_ProfilingEvent_PreprocStop);
                LRF_Interface_Generic_sendOpRx();
            }
        }
    }
    else
    {
        if (lrfEvents.rxOk != 0U || lrfEvents.rxNok != 0U || lrfEvents.rxBufFull != 0U)
        {
            /* Copy received packet from LRF FIFO to buffer */
            /* First, check that there is actually a buffer available */
            while (LRF_hasRxWordToRead() == true)
            {
                /* Check length of received buffer by peeking */
                uint32_t fifoWord = LRF_peekRxFifo(0);
                uint32_t wordLength = RCL_Buffer_DataEntry_paddedLen(fifoWord & 0xFFFFU) / 4U;
                if (wordLength > 0U)
                {
                    if (rxCmd->config.discardRxPackets == 0U)
                    {
                        RCL_MultiBuffer *curBuffer;
                        curBuffer = RCL_MultiBuffer_getBuffer(genericHandlerState.common.curBuffer,
                                                            wordLength * 4U);
                        if (curBuffer != genericHandlerState.common.curBuffer)
                        {
                            rclEvents.rxBufferFinished = 1U;
                            genericHandlerState.common.curBuffer = curBuffer;
                        }
                        if (curBuffer == NULL)
                        {
                            /* Error */
                            genericHandlerState.common.endStatus = RCL_CommandStatus_Error_RxBufferCorruption;
                            /* Send abort */
                            LRF_Interface_Generic_sendOpStop();
                            /* Do not check for more packets from the RX FIFO */
                            break;
                        }
                        else
                        {
                            uint32_t *data32;
                            data32 = (uint32_t *)RCL_MultiBuffer_getNextWritableByte(curBuffer);
                            LRF_readRxFifoWords(data32, wordLength);
                            RCL_MultiBuffer_commitBytes(curBuffer, wordLength * 4U);
                            /* Raise event */
                            rclEvents.rxEntryAvail = 1;
                            /* Adjust effective FIFO size */
                            RCL_Handler_Generic_updateRxCurBufferAndFifo(&rxCmd->rxBuffers);
                        }
                    }
                    else
                    {
                        LRF_discardRxFifoWords(wordLength);
                    }
                }
            }
            if (genericHandlerState.common.activeUpdate)
            {
                RCL_Handler_Generic_updateRxStats(rxCmd->stats, rclSchedulerState.actualStartTime);
            }
            else
            {
                RCL_Handler_Generic_updateLongStats();
            }
        }
        if (rclEventsIn.timerStart != 0U)
        {
            rclEvents.cmdStarted = 1U;
        }
        if (lrfEvents.opDone != 0U || lrfEvents.opError != 0U)
        {
            RCL_CommandStatus endStatus = genericHandlerState.common.endStatus;
            rclEvents.lastCmdDone = 1U;
            if (lrfEvents.opError != 0U && endStatus == RCL_CommandStatus_Finished)
            {
                endStatus = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
            }
            else if (LRF_Interface_isCmdEndCauseEopStop() == true)
            {
                endStatus = RCL_Scheduler_findStopStatus(RCL_StopType_Graceful);
            }
            else
            {
                /* Nothing to do */
            }
            cmd->status = endStatus;
            RCL_Profiling_eventHook(RCL_ProfilingEvent_PostprocStart);
        }
        else
        {
            /* Other events need to be handled unconditionally */
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (rclEventsIn.rxBufferUpdate != 0U)
        {
            RCL_Handler_Generic_updateRxCurBufferAndFifo(&rxCmd->rxBuffers);
        }
    }

    if (rclEvents.lastCmdDone != 0U)
    {
        LRF_disable();
        RCL_Handler_Generic_setSynthPowerState((bool) rxCmd->config.fsOff);
        RCL_Handler_Generic_updateRxStats(rxCmd->stats, rclSchedulerState.actualStartTime);
        /* Restore sync search only if the sync search was disabled */
        LRF_Interface_Generic_restoreSyncSearch(&(genericHandlerState.rx.syncSearchCtrl));
    }

    return rclEvents;
}

/*
 *  ======== RCL_Handler_Generic_LrfOperation ========
 */
RCL_Events RCL_Handler_Generic_LrfOperation(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_Events rclEvents = {.value = 0U};
    RCL_CmdGenericLrfOperation *lrfCmd = (RCL_CmdGenericLrfOperation *) cmd;

    if (cmd->status == RCL_CommandStatus_Scheduled)
    {
        /* Enable radio */
        LRF_enable();

        /* Mark as active */
        cmd->status = RCL_CommandStatus_Active;

        RCL_CommandStatus startTimeStatus = RCL_Scheduler_setCmdStopTimeNoStartTrigger(cmd);
        if (startTimeStatus >= RCL_CommandStatus_Finished)
        {
            cmd->status = startTimeStatus;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Enable interrupts */
            LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);

            /* Post cmd */
            LRF_waitForTopsmReady();
            LRF_Interface_sendOp(lrfCmd->lrfOperation);
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (lrfEvents.opDone != 0U)
        {
            cmd->status = RCL_CommandStatus_Finished;
            rclEvents.lastCmdDone = 1U;
        }
        else if (lrfEvents.opError != 0U)
        {
            cmd->status = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Other events need to be handled unconditionally */
        }
    }
    if (rclEvents.lastCmdDone != 0U)
    {
        LRF_disable();
    }
    return rclEvents;
}


/*
 *  ======== RCL_Handler_Nesb_Ptx ========
 */
RCL_Events RCL_Handler_Nesb_Ptx(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_CmdNesbPtx *txCmd = (RCL_CmdNesbPtx *) cmd;
    RCL_Events rclEvents = {.value = 0U};
    bool runTx = false;
    bool listenAck = false;
    uint32_t earliestStartTime = 0U;

    if (rclEventsIn.setup != 0U)
    {
        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();

        if ((txCmd->rfFrequency == 0U) && (LRF_Interface_Generic_isFreqSynthLocked() == false))
        {
            /* Synth not to be programmed, but not already locked */
            cmd->status = RCL_CommandStatus_Error_Synth;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Program the sync word provided by the radio command to the LRF */
            LRF_Interface_Generic_programSyncWordA(txCmd->syncWord);
            LRF_Interface_Generic_programSyncWordB(txCmd->syncWord);

            /* Configure the LRF for running operation NESB ptx */
            LRF_Interface_Generic_configOpNesbPtx(txCmd->config.fsOff, txCmd->rfFrequency, txCmd->config.autoRetransmitMode);

            /* Mark as active */
            cmd->status = RCL_CommandStatus_Active;

            /* Default end status */
            genericHandlerState.common.endStatus = RCL_CommandStatus_Finished;
            genericHandlerState.tx.txCount = 0U;

            /* Program frequency word */
            if (txCmd->rfFrequency != 0U)
            {
                LRF_programFrequency(txCmd->rfFrequency, true);
            }
            if (LRF_programTxPower(txCmd->txPower, txCmd->rfFrequency) != TxPowerResult_Ok)
            {
                cmd->status = RCL_CommandStatus_Error_Param;
                rclEvents.lastCmdDone = 1U;
            }

            /* Enable radio */
            LRF_enable();

            /* Initialize Tx FIFO */
            genericHandlerState.common.txFifoSize = (uint16_t) LRF_prepareTxFifo();
            rclGenericTxResetCountStops();

            /* Enter header and get ACK configuration */
            RCL_Handler_Nesb_updateHeader(&txCmd->txBuffers,
                                          txCmd->config.autoRetransmitMode,
                                          txCmd->config.hdrConf,
                                          txCmd->seqNo);

            /* Configure Rx if necessary */
            if (LRF_Interface_Generic_shouldNesbListenForAck(txCmd->config.autoRetransmitMode) == true)
            {
                /* Initialize Rx FIFO */
                genericHandlerState.common.rxFifoSize = (uint16_t) LRF_prepareRxFifo();
                genericHandlerState.common.curBuffer = NULL;

                /* Request notification on RX buffer updates */
                RCL_Handler_Generic_updateRxCurBufferAndFifo(&txCmd->rxBuffers);
                listenAck = true;
            }

            /* Enter payload */
            uint32_t nBuffer = RCL_Handler_Generic_updateTxBuffers(&txCmd->txBuffers, 1U);
            if (nBuffer == 0U)
            {
                cmd->status = RCL_CommandStatus_Error_MissingTxBuffer;
                rclEvents.lastCmdDone = 1U;
            }
            else
            {
                RCL_CommandStatus startTimeStatus = RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime);
                if (startTimeStatus >= RCL_CommandStatus_Finished)
                {
                    cmd->status = startTimeStatus;
                    rclEvents.lastCmdDone = 1U;
                }
                else
                {
                    genericHandlerState.common.activeUpdate = RCL_Handler_Nesb_initStats(txCmd->stats,
                                                            rclSchedulerState.actualStartTime);
                    runTx = true;
                }
            }
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        /* We only get an Rx LRF event if an Acknowledge is expected */
        if (lrfEvents.rxOk != 0U || lrfEvents.rxNok != 0U || lrfEvents.rxIgnored != 0U || lrfEvents.rxBufFull != 0U)
        {
            /* Copy received packet from LRF FIFO to buffer */
            /* First, check that there is actually a buffer available */
            while (LRF_hasRxWordToRead() == true)
            {
                /* Check length of received buffer by peeking */
                uint32_t fifoWord = LRF_peekRxFifo(0);
                uint32_t numWords = RCL_Buffer_DataEntry_paddedLen(fifoWord & 0xFFFFU) / sizeof(uint32_t);
                if (numWords > 0U)
                {
                    RCL_MultiBuffer *curBuffer;
                    curBuffer = RCL_MultiBuffer_getBuffer(genericHandlerState.common.curBuffer,
                                                          numWords * sizeof(uint32_t));

                    if (curBuffer != genericHandlerState.common.curBuffer)
                    {
                        rclEvents.rxBufferFinished = 1U;
                        genericHandlerState.common.curBuffer = curBuffer;
                    }

                    if (curBuffer == NULL)
                    {
                        /* Error */
                        genericHandlerState.common.endStatus = RCL_CommandStatus_Error_RxBufferCorruption;
                        /* Send abort */
                        LRF_Interface_Generic_sendOpStop();
                        /* Do not check for more packets from the RX FIFO */
                        break;
                    }
                    else
                    {
                        uint32_t *buffer32 = (uint32_t *)RCL_MultiBuffer_getNextWritableByte(curBuffer);
                        LRF_readRxFifoWords(buffer32, numWords);
                        RCL_MultiBuffer_commitBytes(curBuffer, numWords * sizeof(uint32_t));
                        /* Raise event */
                        rclEvents.rxEntryAvail = 1U;
                        /* Adjust effective FIFO size */
                        RCL_Handler_Generic_updateRxCurBufferAndFifo(&txCmd->rxBuffers);
                    }
                }
            }
            if (genericHandlerState.common.activeUpdate)
            {
                RCL_Handler_Nesb_updateStats(txCmd->stats, rclSchedulerState.actualStartTime);
            }
            else
            {
                RCL_Handler_Nesb_updateLongStats();
            }
        }
        if (rclEventsIn.timerStart != 0U)
        {
            rclEvents.cmdStarted = 1U;
        }
        if (lrfEvents.opDone != 0U)
        {
            /* Retry TX FIFO */
            LRF_retryTxFifo();

            uint16_t lrfCmdEndCause = LRF_Interface_getCmdEndCause();
            if (LRF_Interface_isCmdEndCauseEndOk(lrfCmdEndCause) == true)
            {
                /* Increment the sequence number for next packet */
                txCmd->seqNo = (txCmd->seqNo + 1U) % 4U;

                cmd->status = genericHandlerState.common.endStatus;
                rclEvents.lastCmdDone = 1U;

                /* Pop transmitted packet */
                RCL_Buffer_TxBuffer *txBuffer;
                txBuffer = RCL_TxBuffer_get(&txCmd->txBuffers);
                if (txBuffer != NULL)
                {
                    txBuffer->state = RCL_BufferStateFinished;
                    runTx = false;
                }
                RCL_Profiling_eventHook(RCL_ProfilingEvent_PostprocStart);
            }
            /* Handle missed ACKs or ACKs with the wrong address */
            else if (LRF_Interface_isCmdEndCauseEndedWithoutSync(lrfCmdEndCause) == true)
            {
                /* Configure LRF for next tx operation to retransmit the packet */
                LRF_Interface_Generic_configNextOpTx();

                /* Attempt to retransmit the packet */
                if (genericHandlerState.tx.txCount <= txCmd->maxRetrans)
                {
                    Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Nesb_Ptx: PTX needs to retransmit");

                    /* Set a new transmit time according to retransDelay. If unattainable, retransmit as soon as possible */
                    RCL_CommandStatus startTimeStatus = RCL_Scheduler_setNewStartRelTime(txCmd->retransDelay);
                    if (startTimeStatus >= RCL_CommandStatus_Finished)
                    {
                        Log_printf(LogModule_RCL, Log_WARNING, "RCL_Handler_Nesb_Ptx: Unattainable retranmission delay. Retransmitting as soon as possible");
                        (void) RCL_Scheduler_setNewStartNow();
                    }
                    runTx = true;
                }
                else /* Finish the command without incrementing the sequence number */
                {
                    genericHandlerState.common.endStatus = RCL_CommandStatus_NoSync;
                    cmd->status = genericHandlerState.common.endStatus;
                    rclEvents.lastCmdDone = 1U;
                    runTx = false;
                }
            }
            else
            {
                /* Nothing to do */
            }
        }
        else if (lrfEvents.opError != 0U)
        {
            RCL_CommandStatus endStatus = genericHandlerState.common.endStatus;

            if (endStatus == RCL_CommandStatus_Finished)
            {
                cmd->status = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
            }
            else
            {
                cmd->status = endStatus;
            }
            rclEvents.lastCmdDone = 1U;
            runTx = false;
        }
        else
        {
            /* Other events need to be handled unconditionally */
        }
        if (runTx)
        {
            uint32_t txCount = genericHandlerState.tx.txCount;
            txCount++;
            if (txCount != 0U)
            {
                /* Avoid wraparound */
                genericHandlerState.tx.txCount = txCount;
            }
            /* Set up sync found capture */
            RCL_Hal_setupSyncFoundCap();
            /* Enable interrupts */
            if (listenAck)
            {
                uint16_t fifoCfg = LRF_Interface_Generic_getFifoCfg();
                LRF_enableHwInterrupt(LRF_Interface_Generic_maskEventsByFifoConf(LRF_EventOpDone.value | LRF_EventOpError.value |
                                                                                 LRF_EventRxOk.value | LRF_EventRxNok.value |
                                                                                 LRF_EventRxIgnored.value | LRF_EventRxBufFull.value,
                                                                                 fifoCfg,
                                                                                 genericHandlerState.common.activeUpdate));
            }
            else
            {
                LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);
            }
            /* Post cmd */
            Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Nesb_Ptx: Start of PTX operation");
            LRF_waitForTopsmReady();
            RCL_Profiling_eventHook(RCL_ProfilingEvent_PreprocStop);
            LRF_Interface_Generic_sendOpTx();
        }
    }
    if (rclEvents.lastCmdDone != 0U)
    {
        LRF_disable();
        RCL_Handler_Generic_setSynthPowerState((bool) txCmd->config.fsOff);
        RCL_Handler_Nesb_updateStats(txCmd->stats, rclSchedulerState.actualStartTime);
    }
    return rclEvents;
}


/*
 *  ======== RCL_Handler_Nesb_Prx  ========
 */
RCL_Events RCL_Handler_Nesb_Prx(RCL_Command *cmd, LRF_Events lrfEvents,  RCL_Events rclEventsIn)
{
    RCL_CmdNesbPrx *rxCmd = (RCL_CmdNesbPrx *) cmd;
    RCL_Events rclEvents = RCL_EventNone;

    if (rclEventsIn.setup != 0U)
    {
        uint32_t earliestStartTime;

        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();

        /* Reset sync search control state in case the RF frequency was 0 without the synth running */
        genericHandlerState.nesb.syncSearchCtrl = (LRF_SyncSearchCtrl) { 0U };

        if ((rxCmd->rfFrequency == 0U) && (LRF_Interface_Generic_isFreqSynthLocked() == false))
        {
            /* Synth not to be programmed, but not already locked */
            cmd->status = RCL_CommandStatus_Error_Synth;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Program sync words provided by the radio command to the LRF */
            LRF_Interface_Generic_programSyncWordA(rxCmd->syncWordA);
            LRF_Interface_Generic_programSyncWordB(rxCmd->syncWordB);

            uint16_t lrfOpCfg = LRF_Interface_Generic_getOpCfgNesbPrx(rxCmd->config.fsOff,
                                                                      rxCmd->rfFrequency,
                                                                      rxCmd->config.repeatNok,
                                                                      rxCmd->config.repeatOk);
            /* NOTE: rxCmd->syncWord is an array containing two sync word configurations, i.e. RCL_ConfigAddress */
            LRF_Interface_Generic_configOpNesbPrx(lrfOpCfg, rxCmd->addrLen, rxCmd->syncWord);

            /* Disable sync search for syncwordA and syncwordB if requested by the radio configuration */
            bool disableSyncA = (rxCmd->config.disableSyncA != 0U);
            bool disableSyncB = (rxCmd->config.disableSyncB != 0U);
            /* To avoid the overhead of calling functions by checking the parameters first */
            if (disableSyncA == true || disableSyncB == true)
            {
                LRF_Interface_Generic_disableSyncSearch(disableSyncA, disableSyncB, &(genericHandlerState.nesb.syncSearchCtrl));
            }

            /* Mark as active */
            cmd->status = RCL_CommandStatus_Active;
            /* Default end status */
            genericHandlerState.common.endStatus = RCL_CommandStatus_Finished;

            /* Program frequency word */
            if (rxCmd->rfFrequency != 0U)
            {
                LRF_programFrequency(rxCmd->rfFrequency, false);
            }
            if (LRF_programTxPower(rxCmd->txPower, rxCmd->rfFrequency) != TxPowerResult_Ok)
            {
                cmd->status = RCL_CommandStatus_Error_Param;
                rclEvents.lastCmdDone = 1U;
            }

            /* Enable radio */
            LRF_enable();

            RCL_CommandStatus startTimeStatus = RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime);
            if (startTimeStatus >= RCL_CommandStatus_Finished)
            {
                cmd->status = startTimeStatus;
                rclEvents.lastCmdDone = 1U;
            }
            else
            {
                genericHandlerState.common.activeUpdate = RCL_Handler_Nesb_initStats(rxCmd->stats,
                                                                                     rclSchedulerState.actualStartTime);

                /* Set up sync found capture */
                RCL_Hal_setupSyncFoundCap();
                /* Initialize Rx FIFO */
                genericHandlerState.common.rxFifoSize = (uint16_t) LRF_prepareRxFifo();
                genericHandlerState.common.curBuffer = NULL;

                if (rxCmd->config.discardRxPackets == 0U)
                {
                    RCL_Handler_Generic_updateRxCurBufferAndFifo(&rxCmd->rxBuffers);
                }
                else
                {
                    /* Set FIFO size to maximum */
                    LRF_setRxFifoEffSz(genericHandlerState.common.rxFifoSize);
                }

                /* If an ACK is required, prepare Tx FIFOs */
                if ((rxCmd->syncWord[0].autoAckMode != 0U) || (rxCmd->syncWord[1].autoAckMode != 0U))
                {
                    genericHandlerState.common.txFifoSize = (uint16_t) LRF_prepareTxFifo();
                }

                /* Enable interrupts */
                uint16_t fifoCfg = LRF_Interface_Generic_getFifoCfg();
                LRF_enableHwInterrupt(LRF_Interface_Generic_maskEventsByFifoConf(LRF_EventOpDone.value | LRF_EventOpError.value |
                                                                                 LRF_EventRxOk.value | LRF_EventRxNok.value |
                                                                                 LRF_EventRxIgnored.value | LRF_EventRxBufFull.value,
                                                                                 fifoCfg,
                                                                                 genericHandlerState.common.activeUpdate));

                /* Post cmd */
                Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Nesb_Prx: Starting of PRX operation");
                LRF_waitForTopsmReady();
                RCL_Profiling_eventHook(RCL_ProfilingEvent_PreprocStop);
                LRF_Interface_Generic_sendOpRx();
            }
        }
    }
    else
    {
        if (lrfEvents.rxOk != 0U || lrfEvents.rxNok != 0U || lrfEvents.rxIgnored != 0U || lrfEvents.rxBufFull != 0U)
        {
#if (DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX)
            if (rclFeatureControl.enablePaEsdProtection)
            {
                LRF_updatePaEsdProtection();
            }
#endif
            /* Copy received packet from LRF FIFO to buffer */
            /* First, check that there is actually a buffer available */
            while (LRF_hasRxWordToRead() == true)
            {
                /* Check length of received buffer by peeking */
                uint32_t fifoWord = LRF_peekRxFifo(0);
                uint32_t numWords = RCL_Buffer_DataEntry_paddedLen(fifoWord & 0xFFFFU) / sizeof(uint32_t);
                if (numWords > 0U)
                {
                    if (rxCmd->config.discardRxPackets == 0U)
                    {
                        RCL_MultiBuffer *curBuffer;
                        curBuffer = RCL_MultiBuffer_getBuffer(genericHandlerState.common.curBuffer,
                                                              numWords * 4U);
                        if (curBuffer != genericHandlerState.common.curBuffer)
                        {
                            rclEvents.rxBufferFinished = 1U;
                            genericHandlerState.common.curBuffer = curBuffer;
                        }
                        if (curBuffer == NULL)
                        {
                            /* Error */
                            genericHandlerState.common.endStatus = RCL_CommandStatus_Error_RxBufferCorruption;
                            /* Send abort */
                            LRF_Interface_Generic_sendOpStop();
                            /* Do not check for more packets from the RX FIFO */
                            break;
                        }
                        else
                        {
                            uint32_t *buffer32 = (uint32_t *)RCL_MultiBuffer_getNextWritableByte(curBuffer);
                            LRF_readRxFifoWords(buffer32, numWords);
                            RCL_MultiBuffer_commitBytes(curBuffer, numWords * sizeof(uint32_t));
                            /* Raise event */
                            rclEvents.rxEntryAvail = 1U;
                            /* Adjust effective FIFO size */
                            RCL_Handler_Generic_updateRxCurBufferAndFifo(&rxCmd->rxBuffers);

                            /* NOTE: rxCmd->syncWord is an array containing two sync word configurations,
                               i.e. RCL_ConfigAddress */
                            LRF_Interface_Generic_updateSyncWordCfg(rxCmd->syncWord);
                        }
                    }
                    else
                    {
                        LRF_discardRxFifoWords(numWords);
                    }
                }
            }
            if (genericHandlerState.common.activeUpdate)
            {
                RCL_Handler_Nesb_updateStats(rxCmd->stats, rclSchedulerState.actualStartTime);
            }
            else
            {
                RCL_Handler_Nesb_updateLongStats();
            }
        }
        if (rclEventsIn.timerStart != 0U)
        {
            rclEvents.cmdStarted = 1U;
        }
        if (lrfEvents.opDone != 0U || lrfEvents.opError != 0U)
        {
            RCL_CommandStatus endStatus = genericHandlerState.common.endStatus;

            rclEvents.lastCmdDone = 1U;
            if (lrfEvents.opError != 0U && endStatus == RCL_CommandStatus_Finished)
            {
                endStatus = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
            }
            else if (LRF_Interface_isCmdEndCauseEopStop() == true)
            {
                endStatus = RCL_Scheduler_findStopStatus(RCL_StopType_Graceful);
            }
            else
            {
                /* Nothing to do */
            }
            cmd->status = endStatus;
            RCL_Profiling_eventHook(RCL_ProfilingEvent_PostprocStart);
        }
        else
        {
            /* Other events need to be handled unconditionally */
        }
    }
    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (rclEventsIn.rxBufferUpdate != 0U)
        {
            RCL_Handler_Generic_updateRxCurBufferAndFifo(&rxCmd->rxBuffers);
        }
    }
    if (rclEvents.lastCmdDone != 0U)
    {
        LRF_disable();
        RCL_Handler_Generic_setSynthPowerState((bool) rxCmd->config.fsOff);
        RCL_Handler_Nesb_updateStats(rxCmd->stats, rclSchedulerState.actualStartTime);
        /* Restore sync search only if the sync search was disabled */
        LRF_Interface_Generic_restoreSyncSearch(&(genericHandlerState.nesb.syncSearchCtrl));
    }

    return rclEvents;
}

#if (DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX)

/*
 *  ======== RCL_Handler_Generic_findRxStatusPositionFromEnd ========
 *
 *  Position of the PBE status byte counted back from the end of an RX FIFO
 *  entry body, or 0 if the settings do not append a status byte.
 *
 *  The PBE appends the enabled fields in the order CRC, STATUS, LQI, FREQEST,
 *  RSSI, TIMESTAMP, so the status byte is the first of them and its distance
 *  from the end is its own size plus the size of everything after it. This is
 *  the same rule RCL_Handler_BLE5_findNumExtraBytes and
 *  RCL_Handler_Ieee_findNumExtraBytes apply for their appended fields.
 */
static uint32_t RCL_Handler_Generic_findRxStatusPositionFromEnd(void)
{
    uint16_t fifoCfg = HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_FIFOCFG);
    uint32_t positionFromEnd = 0U;

    if ((fifoCfg & PBE_GENERIC_RAM_FIFOCFG_APPENDSTATUS_M) == 0U)
    {
        return 0U;
    }

    if ((fifoCfg & PBE_GENERIC_RAM_FIFOCFG_APPENDTIMESTAMP_M) != 0U)
    {
        positionFromEnd += sizeof(uint32_t);
    }
    if ((fifoCfg & PBE_GENERIC_RAM_FIFOCFG_APPENDRSSI_M) != 0U)
    {
        positionFromEnd += 1U;
    }
    if ((fifoCfg & PBE_GENERIC_RAM_FIFOCFG_APPENDFREQEST_M) != 0U)
    {
        positionFromEnd += 1U;
    }
    if ((fifoCfg & PBE_GENERIC_RAM_FIFOCFG_APPENDLQI_M) != 0U)
    {
        positionFromEnd += 1U;
    }

    /* The status byte itself */
    positionFromEnd += 1U;

    return positionFromEnd;
}

/*
 *  ======== rclGenericBurstForceNoTxFifoCmd ========
 *
 *  Hold OPCFG.TXFCMD at NONE, so that the PBE issues no TX FIFO command while
 *  the DMA has the FIFO.
 *
 *  Written on every burst rather than only when the static configuration is
 *  programmed. A chained burst (config.reconfigure == 0) does not rewrite
 *  OPCFG, and the PBE does not clear it when it tears an operation down, so a
 *  burst that follows an ordinary generic command would otherwise inherit that
 *  command's TXFCMD and the PBE would issue a FIFO command per packet. Such a
 *  command and the DMA's write to TXFHWR can fall in the same cycle, and the
 *  port write is the one that wins, so the FIFO command is the one lost. That
 *  is the RCL-367 failure mode on the transmit side.
 */
static void rclGenericBurstForceNoTxFifoCmd(void)
{
    uint16_t opcfg = HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_OPCFG);
    HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_OPCFG) =
        (uint16_t) ((opcfg & (uint16_t) ~PBE_GENERIC_RAM_OPCFG_TXFCMD_M)
                    | PBE_GENERIC_RAM_OPCFG_TXFCMD_NONE);
}

/*
 *  ======== RCL_Handler_Generic_TxBurst ========
 */
RCL_Events RCL_Handler_Generic_TxBurst(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_CmdGenericTxBurst *txCmd = (RCL_CmdGenericTxBurst *) cmd;
    RCL_Events rclEvents = {.value = 0U};

    if (rclEventsIn.setup != 0U)
    {
        uint32_t earliestStartTime;

        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();

        if ((txCmd->rfFrequency == 0U) && (LRF_Interface_Generic_isFreqSynthLocked() == false))
        {
            /* Synth not to be programmed, but not already locked */
            cmd->status = RCL_CommandStatus_Error_Synth;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Mark as active */
            cmd->status = RCL_CommandStatus_Active;
            genericHandlerState.common.endStatus = RCL_CommandStatus_Finished;

            /* Static per-configuration programming. These values are constant
             * across a chain of bursts that share sync word, TX power, packet
             * length and packet count, so a chained burst may skip them
             * (config.reconfigure == 0) and reprogram only the frequency below.
             * When reconfigure is 0 the caller guarantees those fields still
             * match the previous burst. */
            if (txCmd->config.reconfigure != 0U)
            {
                /* Program the sync word */
                LRF_Interface_Generic_programSyncWordA(txCmd->syncWord);

                /* Disable NESB. configOpTx is not called: its only other effect
                 * is writing OPCFG, which the burst OPCFG below overwrites. */
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NESB) = PBE_GENERIC_RAM_NESB_NESBMODE_OFF;

                /* Program TX power */
                if (LRF_programTxPower(txCmd->txPower, txCmd->rfFrequency) != TxPowerResult_Ok)
                {
                    cmd->status = RCL_CommandStatus_Error_Param;
                    rclEvents.lastCmdDone = 1U;
                }

                /* Program OPCFG for autonomous burst operation */
                uint16_t opcfg = PBE_GENERIC_RAM_OPCFG_SINGLE_DIS         /* SINGLE=0: multi-packet loop */
                               | PBE_GENERIC_RAM_OPCFG_IFSPERIOD_EN       /* use PRETXIFS for inter-packet gap */
                               | PBE_GENERIC_RAM_OPCFG_NEXTOP_SAME        /* stay on TX after each packet */
                               | PBE_GENERIC_RAM_OPCFG_FS_KEEPON_YES      /* keep PLL on between packets */
                               | PBE_GENERIC_RAM_OPCFG_FS_NOCAL_CAL;      /* calibrate FS (safe default) */

                /* TXFCMD is left out here and forced to NONE on every burst
                 * by rclGenericBurstForceNoTxFifoCmd below. How the space of a
                 * sent packet is given back does not need a FIFO command:
                 * FCFG0.TXADEAL keeps TXFSRP at TXFRP all the time, which is
                 * what lets the FIFO ask for the next entry while the current
                 * one drains, so the command would only repeat what the
                 * hardware already does. */
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_OPCFG) = opcfg;

                /* Configure operation so that it doesn't stop after each TX_DONE */
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_SEQSTAT0) = PBE_GENERIC_RAM_SEQSTAT0_STOPAUTO_NEVER;

                /* Program fixed packet length */
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_MAXLEN) = txCmd->packetLength;
            }

            /* Program frequency word. Reprogrammed every burst: the frequency may
             * change from one chained burst to the next. */
            if (txCmd->rfFrequency != 0U)
            {
                LRF_programFrequency(txCmd->rfFrequency, true);
            }

            /* Enable radio. Skipped for a chained burst that inherits an already
             * enabled LRF from the previous command (config.enableLRF == 0). */
            if (txCmd->config.enableLRF != 0U)
            {
                LRF_enable();
            }

            /* Initialize RF FIFO */
            genericHandlerState.common.txFifoSize = (uint16_t) LRF_prepareTxFifo();

            /* Reset the transmitted-packet counter. Required every burst: the PBE
             * does not clear it between operations, the packet-count stop below
             * counts on it, and it is reported through %stats. */
            LRF_Interface_Generic_setNumOfTxPackets(0U);

            /* Let the PBE end the burst itself once it has sent %numPackets.
             * Without this the operation only ends on a stop time the submitter
             * has to place inside a window one packet wide: too early and the
             * burst is short, too late and the PBE reads a packet that is not
             * there, which sends it into an error path that waits forever for a
             * front end reply. */
            rclGenericTxResetCountStops();
            HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NTXTARGET) = txCmd->numPackets;

            rclGenericBurstForceNoTxFifoCmd();

            /* Nothing is entered into the FIFO here. The DMA moves one entry
             * in each time the FIFO asks for one, from the entries the
             * application posted with RCL_Dma_putTxBurst(), so the FIFO
             * holds no more than the packet on the air and the one after
             * it. The TXFIFO_RESET that LRF_prepareTxFifo issued above is
             * the one FIFO command the CPU writes per burst, written with
             * the previous operation ended and the DMA channel released.
             *
             * The arm moves the first entry in now, and the operation is
             * only posted once it is there: the PBE pops that entry's
             * header as soon as it has the operation, without waiting for
             * the FIFO, and an underflow there sends it into an error path
             * that never returns. A burst that has not been posted by now
             * is therefore missing its buffers, and one whose first entry
             * the DMA has not delivered is a FIFO error, both reported
             * here rather than found by the PBE. */
            int_fast16_t armStatus = RCL_Dma_armTx();

            if (armStatus != RCL_Dma_Status_Success)
            {
                cmd->status = (armStatus == RCL_Dma_Status_Error_Fifo) ? RCL_CommandStatus_Error_TxFifo
                                                                       : RCL_CommandStatus_Error_MissingTxBuffer;
                rclEvents.lastCmdDone = 1U;
            }
            else
            {
                RCL_CommandStatus startTimeStatus = RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime);
                if (startTimeStatus >= RCL_CommandStatus_Finished)
                {
                    cmd->status = startTimeStatus;
                    rclEvents.lastCmdDone = 1U;
                }
                else
                {
                    /* Only the end of the operation is of interest; the CPU
                     * is idle between packets. */
                    LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);

                    /* Submit burst to PBE */
                    Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Generic_TxBurst: Starting burst (%u packets, %u Hz)",
                               txCmd->numPackets, txCmd->rfFrequency);
                    LRF_waitForTopsmReady();
                    RCL_Profiling_eventHook(RCL_ProfilingEvent_PreprocStop);
                    LRF_Interface_Generic_sendOpTx();
                }
            }
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (rclEventsIn.timerStart != 0U)
        {
            rclEvents.cmdStarted = 1U;
        }

        /* Handle burst completion or error. A commanded stop is reported
         * through opDone, so the end cause decides the status, not which
         * event fired; otherwise a graceful stop would be reported as
         * Finished. */
        if ((lrfEvents.opDone != 0U) || (lrfEvents.opError != 0U))
        {
            uint16_t endCause = LRF_Interface_getCmdEndCause();

            switch (endCause)
            {
            case LRF_INTERFACE_ENDCAUSE_STAT_EOPSTOP:
                cmd->status = RCL_Scheduler_findStopStatus(RCL_StopType_Graceful);
                break;

            case LRF_INTERFACE_ENDCAUSE_STAT_ERR_STOP:
                cmd->status = RCL_Scheduler_findStopStatus(RCL_StopType_Hard);
                break;

            default:
                if (lrfEvents.opError != 0U)
                {
                    cmd->status = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
                }
                else
                {
                    cmd->status = genericHandlerState.common.endStatus;
                }
                break;
            }
            rclEvents.lastCmdDone = 1U;
            RCL_Profiling_eventHook(RCL_ProfilingEvent_PostprocStart);
        }
        else if (rclEventsIn.gracefulStop != 0U)
        {
            LRF_sendGracefulStop();
        }
        else
        {
            /* Other events */
        }
    }

    if (rclEvents.lastCmdDone != 0U)
    {
        if (txCmd->stats != NULL)
        {
            uint16_t nTx = (uint16_t) HWREG_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NTX);

            if (txCmd->stats->config.accumulate == 0U)
            {
                txCmd->stats->nTx = nTx;
            }
            else
            {
                txCmd->stats->nTx += nTx;
            }
        }

        /* Releases the DMA channel and hands the transmitted entries back */
        RCL_Dma_stop();

        /* Power down the LRF. Skipped when a chained burst follows and needs the
         * LRF left enabled (config.disableLRF == 0). */
        if (txCmd->config.disableLRF != 0U)
        {
            LRF_disable();
        }
        RCL_Handler_Generic_setSynthPowerState((bool) txCmd->config.fsOff);
    }

    return rclEvents;
}

/*
 *  ======== RCL_Handler_Generic_RxBurst ========
 *
 *  The PBE re-arms sync search after each packet on its own and the DMA moves
 *  each entry out of the RX FIFO as it is committed, so the CPU is idle for
 *  the whole burst. The burst ends when numPackets packets have arrived, on a
 *  sync-search timeout (FIRSTRXTIMEOUT/RXTIMEOUT) or on a commanded stop; the
 *  handler then fills in the caller's slotStatus[] from the entries in RAM.
 */
RCL_Events RCL_Handler_Generic_RxBurst(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_CmdGenericRxBurst *rxCmd = (RCL_CmdGenericRxBurst *) cmd;
    RCL_Events rclEvents = {.value = 0U};

    if (rclEventsIn.setup != 0U)
    {
        uint32_t earliestStartTime;

        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();

        /* Validate command parameters */
        if (rxCmd->numPackets == 0U)
        {
            cmd->status = RCL_CommandStatus_Error_Param;
            rclEvents.lastCmdDone = 1U;
        }
        else if (rxCmd->slotStatus == NULL)
        {
            cmd->status = RCL_CommandStatus_Error_Param;
            rclEvents.lastCmdDone = 1U;
        }
        else if ((rxCmd->rfFrequency == 0U) && (LRF_Interface_Generic_isFreqSynthLocked() == false))
        {
            /* Synth not to be programmed, but not already locked */
            cmd->status = RCL_CommandStatus_Error_Synth;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Initialize all slotStatus entries to PENDING */
            for (uint16_t i = 0; i < rxCmd->numPackets; i++)
            {
                rxCmd->slotStatus[i] = RCL_RX_BURST_SLOT_STATUS_PENDING;
            }

            /* Static per-configuration programming. These values are constant
             * across a chain of bursts that share sync word, packet length,
             * packet count and sync-search timeouts, so a chained burst may skip
             * them (config.reconfigure == 0) and reprogram only the frequency
             * below. When reconfigure is 0 the caller guarantees those fields
             * still match the previous burst. */
            if (rxCmd->config.reconfigure != 0U)
            {
                /* Program the sync word */
                LRF_Interface_Generic_programSyncWordA(rxCmd->syncWord);

                /* Configure LRF for a repeated (multi-packet) RX operation. The
                 * inter-packet gap comes from the RF settings (PRERXIFS); the PBE
                 * re-arms sync search after each packet and receives packets
                 * whenever they arrive on air. */
                LRF_Interface_Generic_configOpRx(rxCmd->config.fsOff, rxCmd->rfFrequency, 1, rxCmd->packetLength);

                /* SEQSTAT0.STOPAUTO = NEVER (so the PBE does not stop after first packet) */
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_SEQSTAT0) = PBE_GENERIC_RAM_SEQSTAT0_STOPAUTO_NEVER;

                /* Program FIRSTRXTIMEOUT (max ticks to wait for the first packet; 0 = wait forever) */
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_FIRSTRXTIMEOUT) = (uint16_t) rxCmd->firstPktTimeoutTicks;

                /* Program RXTIMEOUT: max ticks to wait for each subsequent packet's
                 * sync. 0 = wait forever. A non-zero value lets the PBE end the
                 * burst when a slot's packet never arrives (TIMER0 -> NO_SYNC -> OP
                 * end); the burst ends on that slot and the remaining slots are
                 * marked TIMEOUT. Without it a lost packet stalls the burst until
                 * the stop time. */
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_RXTIMEOUT) = (uint16_t) rxCmd->slotTimeoutTicks;
            }

            /* Reset the received-packet counters. Required every burst: the PBE
             * does not clear them between operations, the packet-count stop
             * below counts on them, and NRXOK is reported through %stats. */
            LRF_Interface_Generic_setNumOfRxOkPackets(0U);
            LRF_Interface_Generic_setNumOfNotRxOkPackets(0U);

            /* Let the PBE end the burst itself once %numPackets have arrived,
             * counting both the good and the bad. A burst that loses a packet
             * never reaches the target and still ends on the sync-search timeout
             * programmed above, which is what bounds it; the target is what ends
             * a complete burst at the last packet instead of one slot later. The
             * drain still stops at %numPackets entries, so a burst that overruns
             * cannot write past the caller's buffers or slotStatus[] array. */
            HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXTARGET) = rxCmd->numPackets;

            rclGenericBurstForceNoTxFifoCmd();

            /* Mark as active */
            cmd->status = RCL_CommandStatus_Active;
            genericHandlerState.common.endStatus = RCL_CommandStatus_Finished;

            /* Program frequency word */
            if (rxCmd->rfFrequency != 0U)
            {
                LRF_programFrequency(rxCmd->rfFrequency, false);
            }

            /* Enable radio. Skipped for a chained burst that inherits an already
             * enabled LRF from the previous command (config.enableLRF == 0). */
            if (rxCmd->config.enableLRF != 0U)
            {
                LRF_enable();
            }

            /* Initialize RF FIFO, then work out the exact footprint of one
             * entry as the PBE will write it, since one DMA arbitration has to
             * be one entry. The value stored in the entry length field is:
             *   numPad field (1) + optional padding + optional header + payload +
             *   EXTRABYTES (appended status/LQI/FREQ/RSSI/timestamp).
             * RCL_Buffer_DataEntry_paddedLen() then adds the 2-byte length field
             * and rounds the entry up to the 32-bit boundary. The three envelope
             * inputs (EXTRABYTES, LENOPTPAD, NUMHDRBITS) come from the settings-
             * programmed registers (valid after configOpRx above), so nothing here
             * is PHY-specific. An entry that comes to a power of two is exactly
             * one arbitration; FIFOCFG.APPEND* and LENOPTPAD are the two settings
             * that move it there. */
            genericHandlerState.common.rxFifoSize = (uint16_t) LRF_prepareRxFifo();

            uint32_t rxExtraBytes = HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_EXTRABYTES);
            uint16_t rxFifoCfg = (uint16_t) HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_FIFOCFG);
            uint32_t rxOptPad = ((uint32_t) rxFifoCfg & PBE_GENERIC_RAM_FIFOCFG_LENOPTPAD_M) >> PBE_GENERIC_RAM_FIFOCFG_LENOPTPAD_S;
            uint32_t rxHdrBytes = 0U;
            uint16_t rxOpCfg = (uint16_t) HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_OPCFG);

            if ((rxOpCfg & PBE_GENERIC_RAM_OPCFG_RXINCLUDEHDR_M) != 0U)
            {
                uint16_t rxPktCfg = (uint16_t) HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_PKTCFG);
                uint32_t rxNumHdrBits = ((uint32_t) rxPktCfg & PBE_GENERIC_RAM_PKTCFG_NUMHDRBITS_M) >> PBE_GENERIC_RAM_PKTCFG_NUMHDRBITS_S;
                rxHdrBytes = (rxNumHdrBits + 7U) / 8U; /* ceil(NUMHDRBITS / 8) */
            }

            /* Entry length-field value = body bytes after the length field. */
            uint32_t rxLenField = 1U + rxOptPad + rxHdrBytes + (uint32_t) rxCmd->packetLength + rxExtraBytes;
            uint32_t rxPerPktBytes = RCL_Buffer_DataEntry_paddedLen(rxLenField);
            /* The DMA takes each entry out of the FIFO as the PBE commits
             * it, and what a burst that ended early left behind at op done,
             * into the buffer the application posted with
             * RCL_Dma_putRxBuffer(); see RCL_Dma_armRxBurst. The burst must
             * fit one transfer and the posted buffer, which the arm checks;
             * it need not fit the FIFO. No FIFO pointer is written while the
             * burst is on the air. */
            bool canStart = (RCL_Dma_armRxBurst(rxCmd->numPackets, rxPerPktBytes) == RCL_Dma_Status_Success);

            /* A burst the DMA cannot be armed for is rejected with
             * Error_Param rather than started. */
            RCL_CommandStatus startTimeStatus = canStart ?
                RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime) :
                RCL_CommandStatus_Error_Param;
            if (startTimeStatus >= RCL_CommandStatus_Finished)
            {
                cmd->status = startTimeStatus;
                rclEvents.lastCmdDone = 1U;
            }
            else
            {
                /* Only the end of the operation is of interest; the CPU is
                 * idle between packets. */
                LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);

                /* Submit burst to PBE */
                Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Generic_RxBurst: Starting burst (%u packets, %u Hz)",
                           rxCmd->numPackets, rxCmd->rfFrequency);
                LRF_waitForTopsmReady();
                RCL_Profiling_eventHook(RCL_ProfilingEvent_PreprocStop);
                LRF_Interface_Generic_sendOpRx();
            }
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (rclEventsIn.timerStart != 0U)
        {
            rclEvents.cmdStarted = 1U;
        }

        /* Handle burst completion. A commanded graceful stop (EOPSTOP) or hard stop
         * is delivered as opError with the stop cause in ENDCAUSE, not as opDone,
         * so both events share this completion path: the FIFO must be drained and
         * slotStatus[] populated whether the burst ended on a sync-search timeout
         * or was bounded by a commanded stop. Only a genuinely fatal end cause is
         * reported as an error below. */
        if (lrfEvents.opDone != 0U || lrfEvents.opError != 0U)
        {
            uint16_t slotIndex = 0;

            /* Whatever the stream did not reach is taken out of the FIFO now
             * that the PBE has ended the operation. The entries then lie
             * back to back in the posted RX buffer, already committed, and
             * slotStatus[] is filled in from RAM. */
            const uint8_t *entry;
            uint32_t numBytes = RCL_Dma_finishRxBurst(&entry);
            uint32_t statusFromEnd = RCL_Handler_Generic_findRxStatusPositionFromEnd();

            while ((numBytes >= sizeof(uint16_t)) && (slotIndex < rxCmd->numPackets))
            {
                uint32_t lengthField = (uint32_t) entry[0] | ((uint32_t) entry[1] << 8);
                uint32_t entryBytes = RCL_Buffer_DataEntry_paddedLen(lengthField);

                if (entryBytes > numBytes)
                {
                    break;
                }
                rclEvents.rxEntryAvail = 1U;

                uint8_t statusByte = ((statusFromEnd != 0U) && (lengthField >= statusFromEnd)) ?
                    entry[sizeof(uint16_t) + lengthField - statusFromEnd] : 0U;

                if ((statusByte & PBE_GENERIC_RAM_STATUSBYTE_CRCERROR_M) == 0U)
                {
                    rxCmd->slotStatus[slotIndex] = RCL_RX_BURST_SLOT_STATUS_OK;
                }
                else
                {
                    rxCmd->slotStatus[slotIndex] = RCL_RX_BURST_SLOT_STATUS_CRC_FAIL;
                }
                slotIndex++;
                entry += entryBytes;
                numBytes -= entryBytes;
            }

            /* Slots the burst never reached */
            while (slotIndex < rxCmd->numPackets)
            {
                rxCmd->slotStatus[slotIndex] = RCL_RX_BURST_SLOT_STATUS_TIMEOUT;
                slotIndex++;
            }

            /* Map the PBE end cause to a command status. A commanded stop (EOPSTOP
             * graceful, ERR_STOP hard) or a slot sync-search timeout (RXTIMEOUT/
             * NOSYNC) may be reported through either opDone or opError, so the end
             * cause decides the status, not which event fired. Any other opError
             * cause is a genuine fatal error; otherwise the burst ended normally. */
            uint16_t endCause = LRF_Interface_getCmdEndCause();
            switch (endCause)
            {
            case LRF_INTERFACE_ENDCAUSE_STAT_EOPSTOP:
                cmd->status = RCL_Scheduler_findStopStatus(RCL_StopType_Graceful);
                break;

            case LRF_INTERFACE_ENDCAUSE_STAT_ERR_STOP:
                cmd->status = RCL_Scheduler_findStopStatus(RCL_StopType_Hard);
                break;

            case LRF_INTERFACE_ENDCAUSE_STAT_RXTIMEOUT:
            case LRF_INTERFACE_ENDCAUSE_STAT_NOSYNC:
                /* A slot's sync search timed out (RXTIMEOUT for the first packet,
                 * NOSYNC for a subsequent one). The burst ends on that slot; the
                 * drain has already marked it and the rest TIMEOUT. */
                cmd->status = RCL_CommandStatus_RxTimeout;
                break;

            default:
                if (lrfEvents.opError != 0U)
                {
                    cmd->status = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
                }
                else
                {
                    cmd->status = genericHandlerState.common.endStatus;
                }
                break;
            }
            rclEvents.lastCmdDone = 1U;
            RCL_Profiling_eventHook(RCL_ProfilingEvent_PostprocStart);
        }
        else if (rclEventsIn.gracefulStop != 0U)
        {
            LRF_sendGracefulStop();
        }
        else
        {
            /* Other events need to be handled unconditionally */
        }
    }

    if (rclEvents.lastCmdDone != 0U)
    {
        if (rxCmd->stats != NULL)
        {
            uint16_t nRxOk = HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXOK);

            if (rxCmd->stats->config.accumulate == 0U)
            {
                rxCmd->stats->nRxOk = nRxOk;
            }
            else
            {
                rxCmd->stats->nRxOk += nRxOk;
            }
        }

        /* Releases the DMA channel, the posted RX buffer stays posted */
        RCL_Dma_stop();

        /* Power down the LRF. Skipped when a chained burst follows and needs the
         * LRF left enabled (config.disableLRF == 0). */
        if (rxCmd->config.disableLRF != 0U)
        {
            LRF_disable();
        }
        RCL_Handler_Generic_setSynthPowerState((bool) rxCmd->config.fsOff);
    }

    return rclEvents;
}

/*
 *  ======== rclGenericStreamEnableStopTimeIrqs ========
 *
 *  A stop time, the command's own or the scheduler's, reaches a stream only
 *  as an RCL event: the PBE tests the compares itself at the end of a packet,
 *  but a stream's operation done is masked and there may be no packet, so
 *  the doorbell interrupts of both compares are enabled when the command
 *  starts, wherever a stop time is set. RCL programs the times right after
 *  this, and delivers each as gracefulStop or hardStop to the handler, which
 *  then writes the stop to the PBE like a stop requested through the API.
 */
static void rclGenericStreamEnableStopTimeIrqs(void)
{
    if (rclSchedulerState.gracefulStopInfo.cmdStopEnabled || rclSchedulerState.gracefulStopInfo.schedStopEnabled)
    {
        RCL_Hal_enableGracefulStopTimeIrq();
    }
    if (rclSchedulerState.hardStopInfo.cmdStopEnabled || rclSchedulerState.hardStopInfo.schedStopEnabled)
    {
        RCL_Hal_enableHardStopTimeIrq();
    }
}

/*
 *  ======== rclGenericStreamRequestStop, below ========
 *
 *  Turn a stop request into something the PBE answers, whatever it is doing.
 *
 *  A stream keeps the operation-done interrupt masked, since an operation ends
 *  with every packet and the CPU is not to be woken for it; only an operation
 *  error is serviced. The end of a stream is therefore announced by nothing
 *  unless the handler asks for it here. The PBE answers a stop written to
 *  LRFDPBE.API while it runs an operation with the operation's end, and a
 *  stop written while it is idle with an immediate operation done that leaves
 *  ENDCAUSE untouched, so an operation done is the answer in either case.
 *
 *  A stop requested through the API has already been written to the PBE by
 *  RCL when this runs; a stop delivered by one of the stop timers has not,
 *  RCL only posts the event. The dones of every packet sent so far are still
 *  latched in the doorbell, masked, and would be taken for the answer the
 *  moment the mask is lifted, so they are cleared first. That clear may also
 *  take the answer to a stop RCL wrote, which an idle PBE gives within a few
 *  cycles, and so the stop is written here in every case, after the mask is
 *  lifted: an idle PBE answers it, again if need be, and a running one holds
 *  it, the API register keeping the last value written. At most one answer
 *  is left latched at the end of the command, and RCL clears the doorbell
 *  when a command ends.
 *
 *  A hard stop supersedes a graceful one.
 */
/*
 *  ======== rclGenericStreamNoteStop ========
 *
 *  Record a stop request in the handler state, the harder of it and any
 *  earlier one; what is then done about it is the handler's, below.
 */
static void rclGenericStreamNoteStop(RCL_Events rclEventsIn)
{
    RCL_StopType stopType = (rclEventsIn.hardStop != 0U) ? RCL_StopType_Hard : RCL_StopType_Graceful;

    if (stopType > genericHandlerState.stream.stopType)
    {
        genericHandlerState.stream.stopType = stopType;
    }
}

/*
 *  ======== rclGenericStreamRequestStop ========
 */
static void rclGenericStreamRequestStop(RCL_Events rclEventsIn)
{
    rclGenericStreamNoteStop(rclEventsIn);
    LRF_clearHwInterrupt(LRF_EventOpDone.value);
    LRF_enableHwInterrupt(LRF_EventOpDone.value);
    if (genericHandlerState.stream.stopType == RCL_StopType_Hard)
    {
        LRF_sendHardStop();
    }
    else
    {
        LRF_sendGracefulStop();
    }
}

/*
 *  ======== rclGenericStreamEndStatus ========
 *
 *  Status of a stream whose PBE has reported an operation end. A genuine
 *  error is reported as such whether or not a stop was under way; otherwise
 *  the requested stop decides, since the operation that answered it may have
 *  ended normally, been cut short (ERR_STOP) or finished its packet first
 *  (EOPSTOP), and all three are the same stop. An end cause of EOPSTOP or
 *  ERR_STOP with no stop requested here comes from a stop time the scheduler
 *  delivered straight to the PBE, and maps to the matching stop status. On a
 *  hopping command a stop is never written to the PBE, so the operation
 *  that answers it ends as any other, on its count or on a timeout.
 */
static RCL_CommandStatus rclGenericStreamEndStatus(LRF_Events lrfEvents)
{
    RCL_CommandStatus status;
    uint16_t endCause = LRF_Interface_getCmdEndCause();
    bool stopped = (endCause == LRF_INTERFACE_ENDCAUSE_STAT_EOPSTOP) ||
                   (endCause == LRF_INTERFACE_ENDCAUSE_STAT_ERR_STOP) ||
                   ((genericHandlerState.stream.numHops != 0U) &&
                    (genericHandlerState.stream.stopType != RCL_StopType_None) &&
                    ((endCause == LRF_INTERFACE_ENDCAUSE_STAT_ENDOK) ||
                     (endCause == LRF_INTERFACE_ENDCAUSE_STAT_RXTIMEOUT) ||
                     (endCause == LRF_INTERFACE_ENDCAUSE_STAT_NOSYNC)));

    if ((lrfEvents.opError != 0U) && !stopped)
    {
        status = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
    }
    else if (genericHandlerState.stream.stopType != RCL_StopType_None)
    {
        status = RCL_Scheduler_findStopStatus(genericHandlerState.stream.stopType);
    }
    else
    {
        status = RCL_Handler_Generic_mapLrfErrorStatusToRclStatus();
    }
    return status;
}

/*
 *  ======== rclGenericHopRegs ========
 *
 *  One row of a hop table is everything LRF_programFrequency writes for a
 *  frequency, in the order it writes it: the RFE RAM frequency word K5 and
 *  the coarse and mid calibration dividers; the demodulator resampler
 *  fraction and its two shadow words, DEMCOHR3 and DEMCOHR4, which are only
 *  written when LRFDMDM.BAUDCOMP has FRAC_SHADOW set and read zero
 *  otherwise, carried so that the row is complete for any configuration; the
 *  two PLL words, which also carry the HFXT compensation of the moment; the
 *  RFE RAM IF words; the mixer word and its spare copy; the six TX filter
 *  tap words, which depend on the frequency through the deviation scaling;
 *  and the shaping gain in MOD0. One list both captures a row at setup and
 *  applies it at a hop, so the two cannot drift apart. The RFE RAM words are
 *  halfwords, the rest 32-bit registers.
 */
typedef struct {
    uint32_t addr;
    bool     halfword;
} RclGenericHopReg;

static const RclGenericHopReg rclGenericHopRegs[RCL_GENERIC_HOP_ROW_WORDS] = {
    { LRFD_RFERAM_BASE + RFE_COMMON_RAM_O_K5,           true  },
    { LRFDRFE32_BASE + LRFDRFE32_O_CALMMID_CALMCRS,     false },
    { LRFDMDM32_BASE + LRFDMDM32_O_DEMFRAC1_DEMFRAC0,   false },
    { LRFDMDM32_BASE + LRFDMDM32_O_DEMFRAC3_DEMFRAC2,   false },
    { LRFDMDM_BASE + LRFDMDM_O_DEMCOHR3,                false },
    { LRFDMDM_BASE + LRFDMDM_O_DEMCOHR4,                false },
    { LRFDRFE32_BASE + LRFDRFE32_O_PLLM0,               false },
    { LRFDRFE32_BASE + LRFDRFE32_O_PLLM1,               false },
    { LRFD_RFERAM_BASE + RFE_COMMON_RAM_O_RXIF,         true  },
    { LRFD_RFERAM_BASE + RFE_COMMON_RAM_O_TXIF,         true  },
    { LRFDMDM_BASE + LRFDMDM_O_DEMMISC0,                false },
    { LRFDMDM_BASE + LRFDMDM_O_SPARE3,                  false },
    { LRFDRFE32_BASE + LRFDRFE32_O_DTX1_DTX0,           false },
    { LRFDRFE32_BASE + LRFDRFE32_O_DTX3_DTX2,           false },
    { LRFDRFE32_BASE + LRFDRFE32_O_DTX5_DTX4,           false },
    { LRFDRFE32_BASE + LRFDRFE32_O_DTX7_DTX6,           false },
    { LRFDRFE32_BASE + LRFDRFE32_O_DTX9_DTX8,           false },
    { LRFDRFE32_BASE + LRFDRFE32_O_DTX11_DTX10,         false },
    { LRFDRFE_BASE + LRFDRFE_O_MOD0,                    false },
};

static void rclGenericStreamCaptureRow(uint32_t *row)
{
    for (uint32_t i = 0U; i < RCL_GENERIC_HOP_ROW_WORDS; i++)
    {
        row[i] = rclGenericHopRegs[i].halfword ? HWREGH_READ_LRF(rclGenericHopRegs[i].addr)
                                               : HWREG_READ_LRF(rclGenericHopRegs[i].addr);
    }
}

static void rclGenericStreamApplyRow(const uint32_t *row)
{
    for (uint32_t i = 0U; i < RCL_GENERIC_HOP_ROW_WORDS; i++)
    {
        if (rclGenericHopRegs[i].halfword)
        {
            HWREGH_WRITE_LRF(rclGenericHopRegs[i].addr) = (uint16_t) row[i];
        }
        else
        {
            HWREG_WRITE_LRF(rclGenericHopRegs[i].addr) = row[i];
        }
    }
}

/*
 *  ======== rclGenericStreamHopSetup ========
 *
 *  Take a stream's hop table into the handler state, or none, and start the
 *  hop counters. Returns false for a table the handler cannot use.
 */
static bool rclGenericStreamHopSetup(const uint32_t *hopFrequencies, uint32_t *hopRows, uint8_t numHops, uint8_t packetsPerHop)
{
    genericHandlerState.stream.numHops = 0U;
    genericHandlerState.stream.packetsPerHop = packetsPerHop;
    genericHandlerState.stream.channel = 0U;
    genericHandlerState.stream.target = 0U;
    genericHandlerState.stream.hops = 0U;
    genericHandlerState.stream.hopsMissed = 0U;
    genericHandlerState.stream.rxOk = 0U;
    genericHandlerState.stream.rxNok = 0U;
    genericHandlerState.stream.rows = hopRows;

    if (hopFrequencies == NULL)
    {
        return true;
    }
    if ((hopRows == NULL) || (numHops < 2U) || (packetsPerHop == 0U))
    {
        return false;
    }
    for (uint32_t i = 0U; i < numHops; i++)
    {
        if (hopFrequencies[i] == 0U)
        {
            return false;
        }
    }
    genericHandlerState.stream.numHops = numHops;
    return true;
}

/*
 *  ======== rclGenericStreamProgramFrequency ========
 *
 *  Program a stream's frequency. With a hop table, every channel in turn,
 *  the first one last, each row captured once LRF_programFrequency has
 *  written it, so that the radio is left on the first channel for the
 *  calibration of the setup; without one, the command's frequency if it has
 *  one. Runs at setup with the front end idle, which is when its RAM can be
 *  read back: while the RFE runs, the RAM is its alone and reads return
 *  something else.
 */
static void rclGenericStreamProgramFrequency(uint32_t frequency, const uint32_t *hopFrequencies, bool tx)
{
    uint32_t numHops = genericHandlerState.stream.numHops;

    if (numHops != 0U)
    {
        for (uint32_t i = numHops; i > 0U; i--)
        {
            LRF_programFrequency(hopFrequencies[i - 1U], tx);
            rclGenericStreamCaptureRow(&genericHandlerState.stream.rows[(i - 1U) * RCL_GENERIC_HOP_ROW_WORDS]);
        }
    }
    else if (frequency != 0U)
    {
        LRF_programFrequency(frequency, tx);
    }
    else
    {
        /* The synthesizer is already on the frequency */
    }
}

/*
 *  ======== rclGenericStreamHop ========
 *
 *  Move to the next channel and, if the RFE is idle, write its row into the
 *  radio: the stores of the row and nothing computed. The RFE's RAM is its
 *  alone while it runs, so a hop that finds it running skips the row and
 *  counts a miss; the channel moves on regardless, so that the schedule is
 *  kept and the miss costs the dwell and not the front end. Idle is read
 *  from LRFDPBE.RFEMSGBOX, the RFE's report of its last command: the RFE
 *  clears it when it takes a command (rfe_ram_bank0.asm:117-121, outclr
 *  MSGBOX), about a microsecond after the API write, and writes it when
 *  the command is done. On the TX path the hop interrupt is raised from
 *  the NTX store that TX_DONE reaches only after waiting for the modem's
 *  and the RFE's reports (pbe_ram_bank0.asm:416-430), so the word is set
 *  by then and a post the RFE has taken up since is seen, while one made
 *  in the microsecond before it has is not, which is the application's
 *  posting contract in the command's description. On the RX path the hop
 *  runs after an operation done, and OP_COMMON_END's RESET_ALL has waited
 *  for the RFE's report before writing ENDCAUSE
 *  (pbe_commonlib_reset.asm:26-29), so the test is always true there.
 *  Measured as corroboration: 0 while a packet is on the air, 1 at the hop
 *  interrupt and at the operation done. LRFDRFE32.RFSTATE reads IDLE
 *  throughout with this RFE image and tells nothing.
 */
static void rclGenericStreamHop(void)
{
    uint32_t channel = (uint32_t) genericHandlerState.stream.channel + 1U;

    if (channel >= genericHandlerState.stream.numHops)
    {
        channel = 0U;
    }
    genericHandlerState.stream.channel = (uint8_t) channel;
    genericHandlerState.stream.hops++;
    if (HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_RFEMSGBOX) != 0U)
    {
        rclGenericStreamApplyRow(&genericHandlerState.stream.rows[channel * RCL_GENERIC_HOP_ROW_WORDS]);
    }
    else
    {
        genericHandlerState.stream.hopsMissed++;
    }
}

/*
 *  ======== rclGenericTxStreamConfigOp ========
 *
 *  The OPCFG every packet operation of a TX stream runs with: one packet per
 *  operation, started by the API write itself rather than by a SysTimer
 *  compare, so that the packet leaves a fixed time after whoever posts it
 *  has written the register; the synthesizer already locked and kept on
 *  between operations; and no FIFO command from the PBE at the end of a
 *  packet, since with FCFG0.TXADEAL the FIFO frees the packet's space as it
 *  is modulated, and a FIFO command from the PBE could land in the same cycle
 *  as a data port push and be lost to it, which is RCL-367 on the transmit
 *  side.
 */
static void rclGenericTxStreamConfigOp(void)
{
    HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_OPCFG) =
        PBE_GENERIC_RAM_OPCFG_SINGLE_EN
        | PBE_GENERIC_RAM_OPCFG_START_M
        | PBE_GENERIC_RAM_OPCFG_FS_NOCAL_NOCAL
        | PBE_GENERIC_RAM_OPCFG_FS_KEEPON_YES
        | PBE_GENERIC_RAM_OPCFG_TXFCMD_NONE
        | PBE_GENERIC_RAM_OPCFG_NEXTOP_SAME;
}

/*
 *  ======== RCL_Handler_Generic_TxStream ========
 *
 *  The handler configures the radio and the FIFO and then does nothing per
 *  packet. Each packet is a single-packet operation the application starts
 *  by writing OP_TX to LRFDPBE.API once the entry is in the TX FIFO; with
 *  OPCFG.START asynchronous the PBE starts on that write. The command ends
 *  only on a stop or on an operation error.
 */
RCL_Events RCL_Handler_Generic_TxStream(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_CmdGenericTxStream *txCmd = (RCL_CmdGenericTxStream *) cmd;
    RCL_Events rclEvents = {.value = 0U};

    if (rclEventsIn.setup != 0U)
    {
        uint32_t earliestStartTime;
        /* With a hop table the command starts on its first channel */
        uint32_t frequency = (txCmd->hopFrequencies != NULL) ? txCmd->hopFrequencies[0] : txCmd->rfFrequency;
        bool hopOk = rclGenericStreamHopSetup(txCmd->hopFrequencies, txCmd->hopRows, txCmd->numHops, txCmd->packetsPerHop);

        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();

        if ((frequency == 0U) && (LRF_Interface_Generic_isFreqSynthLocked() == false))
        {
            /* Synth not to be programmed, but not already locked */
            cmd->status = RCL_CommandStatus_Error_Synth;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Mark as active */
            cmd->status = RCL_CommandStatus_Active;
            genericHandlerState.stream.stopType = RCL_StopType_None;

            /* Program the sync word */
            LRF_Interface_Generic_programSyncWordA(txCmd->syncWord);

            /* Disable NESB. configOpTx is not called: its only other effect
             * is writing OPCFG, which the stream OPCFG below overwrites. */
            HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NESB) = PBE_GENERIC_RAM_NESB_NESBMODE_OFF;

            /* Program TX power */
            if (!hopOk || (LRF_programTxPower(txCmd->txPower, frequency) != TxPowerResult_Ok))
            {
                cmd->status = RCL_CommandStatus_Error_Param;
                rclEvents.lastCmdDone = 1U;
            }
            else
            {
                /* Every packet operation runs with the same OPCFG, so a
                 * synthesizer calibration would be in every packet or in
                 * none; measured, it is most of the time from the API write
                 * to the first bit, so it is in none. With a frequency to
                 * program, the calibration is one FS operation of this
                 * command's own, posted below and run by the same TOPSM
                 * state the packets will use, and the packet OPCFG is only
                 * written once it has locked. Without one the synthesizer
                 * has to be locked already, see the command's description. */
                genericHandlerState.stream.calibrating = (frequency != 0U);
                if (genericHandlerState.stream.calibrating)
                {
                    HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_OPCFG) =
                        PBE_GENERIC_RAM_OPCFG_FS_NOCAL_CAL | PBE_GENERIC_RAM_OPCFG_FS_KEEPON_YES;
                }
                else
                {
                    rclGenericTxStreamConfigOp();
                }

                /* Program fixed packet length */
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_MAXLEN) = txCmd->packetLength;

                /* Program the frequency, or every channel of the hop table */
                rclGenericStreamProgramFrequency(frequency, txCmd->hopFrequencies, true);

                /* Enable radio */
                if (txCmd->config.enableLRF != 0U)
                {
                    LRF_enable();
                }

                /* Reset the FIFO, the one FIFO command of the command's life,
                 * written with the PBE idle. LRF_prepareTxFifo leaves auto
                 * deallocate off because the CPU path retries the FIFO to repeat
                 * a packet; nothing is repeated here, and without it the space of
                 * a sent packet would never come back. */
                genericHandlerState.common.txFifoSize = (uint16_t) LRF_prepareTxFifo();
                HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0) =
                    HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0) | LRFDPBE_FCFG0_TXADEAL_M;

                /* Reset the transmitted-packet counter, reported through %stats.
                 * The PBE does not clear it between operations. NTXTARGET is
                 * zeroed as well: this PBE image ends an operation when the count
                 * reaches it, the word lies outside the settings image, and a
                 * stream is not to end on a count. */
                LRF_Interface_Generic_setNumOfTxPackets(0U);
                rclGenericTxResetCountStops();

                /* With a hop table, the PBE raises its interrupt 9 after
                 * every packetsPerHop-th packet and restarts NTX; the hop
                 * is done on that interrupt */
                if (genericHandlerState.stream.numHops != 0U)
                {
                    HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NTXIRQ) = txCmd->packetsPerHop;
                }

                RCL_CommandStatus startTimeStatus = RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime);
                if (startTimeStatus >= RCL_CommandStatus_Finished)
                {
                    cmd->status = startTimeStatus;
                    rclEvents.lastCmdDone = 1U;
                }
                else
                {
                    /* An operation ends with every packet; only an error is of
                     * interest. Nothing is posted here for the packets: the
                     * application posts every operation. The calibration is
                     * the one operation this command posts, started on the
                     * command's start time as any operation is, and its end
                     * is waited for. */
                    Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Generic_TxStream: Radio up (%u Hz)", frequency);
                    LRF_waitForTopsmReady();
                    RCL_Profiling_eventHook(RCL_ProfilingEvent_PreprocStop);
                    if (genericHandlerState.stream.calibrating)
                    {
                        LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);
                        LRF_Interface_Generic_sendOpFs();
                    }
                    else
                    {
                        LRF_enableHwInterrupt(LRF_EventOpError.value);
                    }
                }
            }
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if ((rclEventsIn.timerStart != 0U) && !genericHandlerState.stream.calibrating)
        {
            /* The start time has passed and no operation was waiting for
             * it, so the compare event is still latched in the PBE. An
             * operation tests that event as its hard stop, and one started
             * asynchronously does not consume it, so the first packet would
             * end with ERR_STOP unless it is cleared here. This is why an
             * operation may only be posted once the command has started.
             * A calibration in flight has consumed the compare itself, and
             * the command starts when it has locked. */
            HWREGH_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_EVTCLR0) = LRFDPBE_EVTCLR0_SYSTCMP0_M;
            rclGenericStreamEnableStopTimeIrqs();
            rclEvents.cmdStarted = 1U;
        }

        if ((lrfEvents.value & LRF_EventStreamHop.value) != 0U)
        {
            /* The packetsPerHop-th packet of the dwell has left the air. The
             * RFE is idle by the time the interrupt is raised (measured:
             * 3 us after the PA has gone down, 4 us before the operation
             * done) and the application's next post is a packet gap away,
             * so the next channel's row goes in here and now; if the post
             * came first, or this interrupt was held off until it did, the
             * hop skips the row rather than write into a running RFE. Its
             * own test, so that it is not lost to an operation end read in
             * the same doorbell word. */
            rclGenericStreamHop();
        }

        if ((lrfEvents.opDone != 0U) || (lrfEvents.opError != 0U))
        {
            if (genericHandlerState.stream.calibrating && (lrfEvents.opError == 0U) &&
                (genericHandlerState.stream.stopType == RCL_StopType_None))
            {
                /* Locked. From here the operations are the application's:
                 * the packet OPCFG goes in, the operation done goes back
                 * under its mask, and the command counts as started. */
                genericHandlerState.stream.calibrating = false;
                rclGenericTxStreamConfigOp();
                LRF_disableHwInterrupt(LRF_EventOpDone.value);
                LRF_clearHwInterrupt(LRF_EventOpDone.value);
                if (genericHandlerState.stream.numHops != 0U)
                {
                    /* The hop interrupt latches in the doorbell whether or
                     * not it is enabled, so whatever is latched from before
                     * this command is cleared first */
                    LRF_clearHwInterrupt(LRF_EventStreamHop.value);
                    LRF_enableHwInterrupt(LRF_EventStreamHop.value);
                }
                rclGenericStreamEnableStopTimeIrqs();
                rclEvents.cmdStarted = 1U;
            }
            else
            {
                cmd->status = rclGenericStreamEndStatus(lrfEvents);
                rclEvents.lastCmdDone = 1U;
                RCL_Profiling_eventHook(RCL_ProfilingEvent_PostprocStart);
            }
        }
        else if ((rclEventsIn.gracefulStop != 0U) || (rclEventsIn.hardStop != 0U))
        {
            rclGenericStreamRequestStop(rclEventsIn);
        }
        else
        {
            /* Other events */
        }
    }

    if (rclEvents.lastCmdDone != 0U)
    {
        /* Leave no periodic interrupt behind for the next transmit command */
        HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NTXIRQ) = 0U;

        if (txCmd->stats != NULL)
        {
            /* NTX restarts from zero at every hop, so the packets before the
             * last hop are counted from the hops */
            uint32_t nTx = (genericHandlerState.stream.hops * genericHandlerState.stream.packetsPerHop) +
                           HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NTX);

            if (txCmd->stats->config.accumulate == 0U)
            {
                txCmd->stats->nTx = (uint16_t) nTx;
                txCmd->stats->nHops = (uint16_t) genericHandlerState.stream.hops;
                txCmd->stats->nHopsMissed = (uint16_t) genericHandlerState.stream.hopsMissed;
            }
            else
            {
                txCmd->stats->nTx += (uint16_t) nTx;
                txCmd->stats->nHops += (uint16_t) genericHandlerState.stream.hops;
                txCmd->stats->nHopsMissed += (uint16_t) genericHandlerState.stream.hopsMissed;
            }
        }

        if (txCmd->config.disableLRF != 0U)
        {
            LRF_disable();
        }
        RCL_Handler_Generic_setSynthPowerState((bool) txCmd->config.fsOff);
    }

    return rclEvents;
}

/*
 *  ======== rclGenericRxStreamContinue ========
 *
 *  An operation of a hopping RX stream has ended; post the next one, or
 *  return false if the command is to end instead: on a stop, honoured here
 *  and never written into a running operation, or on a genuine error.
 *
 *  The dwell is over when the operation ended on the count hopSync armed,
 *  when it timed out with no count armed or with nothing heard since its
 *  post (1.25 dwells of silence, whatever a stale target says), or when the
 *  timeout leaves no packet of the dwell to expect: a timeout after packets
 *  is one packet lost, so the packets still expected are the target less
 *  the count less the lost one.
 *  Over, the next channel's row goes in and the operation is posted for the
 *  new dwell with no count; not over, it is posted on the same channel for
 *  the packets still expected. Either way the PBE and the RFE are idle for
 *  the writes, which is the only time the RFE's RAM may be written, and the
 *  post starts the operation at once, with the synthesizer kept on and no
 *  calibration. The counts restart at zero for every operation so that the
 *  target arithmetic never wraps; the ended operation's are added to the
 *  totals here.
 */
static bool rclGenericRxStreamContinue(void)
{
    uint16_t endCause = LRF_Interface_getCmdEndCause();
    bool normalEnd = (endCause == LRF_INTERFACE_ENDCAUSE_STAT_ENDOK) ||
                     (endCause == LRF_INTERFACE_ENDCAUSE_STAT_RXTIMEOUT) ||
                     (endCause == LRF_INTERFACE_ENDCAUSE_STAT_NOSYNC);

    if (!normalEnd || (genericHandlerState.stream.stopType != RCL_StopType_None))
    {
        return false;
    }

    uint32_t nRxOk = HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXOK);
    uint32_t nRxNok = HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXNOK);
    uint32_t count = nRxOk + nRxNok;
    uint32_t target = genericHandlerState.stream.target;
    uint32_t remaining = 0U;

    genericHandlerState.stream.rxOk += nRxOk;
    genericHandlerState.stream.rxNok += nRxNok;
    LRF_Interface_Generic_setNumOfRxOkPackets(0U);
    LRF_Interface_Generic_setNumOfNotRxOkPackets(0U);

    if ((endCause != LRF_INTERFACE_ENDCAUSE_STAT_ENDOK) && (count != 0U) && (target > count + 1U))
    {
        remaining = target - count - 1U;
    }
    if (remaining == 0U)
    {
        rclGenericStreamHop();
    }
    genericHandlerState.stream.target = (uint16_t) remaining;
    HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXTARGET) = (uint16_t) remaining;

    uint16_t opCfg = (uint16_t) HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_OPCFG);
    opCfg &= (uint16_t) ~(PBE_GENERIC_RAM_OPCFG_FS_NOCAL_M | PBE_GENERIC_RAM_OPCFG_FS_KEEPON_M);
    opCfg |= PBE_GENERIC_RAM_OPCFG_FS_NOCAL_NOCAL | PBE_GENERIC_RAM_OPCFG_FS_KEEPON_YES | PBE_GENERIC_RAM_OPCFG_START_M;
    HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_OPCFG) = opCfg;
    LRF_Interface_Generic_sendOpRx();
    return true;
}

/*
 *  ======== RCL_CmdGenericRxStream_hopSync ========
 */
void RCL_CmdGenericRxStream_hopSync(RCL_CmdGenericRxStream *cmd, uint32_t packetCounter)
{
    uint32_t packetsPerHop = genericHandlerState.stream.packetsPerHop;

    /* Nothing to align once a stop is pending: the handler has armed the
     * count that ends the operation with the next packet */
    if ((cmd->common.status != RCL_CommandStatus_Active) || (genericHandlerState.stream.numHops == 0U) ||
        (genericHandlerState.stream.stopType != RCL_StopType_None))
    {
        return;
    }

    uint32_t position = packetCounter % packetsPerHop;

    if (position + 1U < packetsPerHop)
    {
        /* The packets of this operation so far, this one included by the
         * time the entry has been drained, plus the rest of the dwell. Read
         * and written with interrupts off, so that the command's own
         * interrupt cannot end and re-post the operation, and zero the
         * counts, between the read and the write. */
        uintptr_t key = HwiP_disable();
        uint32_t target = HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXOK) +
                          HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXNOK) +
                          (packetsPerHop - 1U - position);

        genericHandlerState.stream.target = (uint16_t) target;
        HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXTARGET) = (uint16_t) target;
        HwiP_restore(key);
    }
}

/*
 *  ======== RCL_Handler_Generic_RxStream ========
 *
 *  One repeated RX operation for the life of the command, or, with a hop
 *  table, one per dwell, each posted by the handler when the previous has
 *  ended. The PBE re-arms
 *  sync search after every packet and commits each entry to the RX FIFO,
 *  and the commit is routed to the LRF DMA trigger for the application's
 *  channel to take the entry out. The command ends only on a stop or on an
 *  operation error.
 */
RCL_Events RCL_Handler_Generic_RxStream(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_CmdGenericRxStream *rxCmd = (RCL_CmdGenericRxStream *) cmd;
    RCL_Events rclEvents = {.value = 0U};

    if (rclEventsIn.setup != 0U)
    {
        uint32_t earliestStartTime;
        /* With a hop table the command starts on its first channel */
        uint32_t frequency = (rxCmd->hopFrequencies != NULL) ? rxCmd->hopFrequencies[0] : rxCmd->rfFrequency;
        bool hopOk = rclGenericStreamHopSetup(rxCmd->hopFrequencies, rxCmd->hopRows, rxCmd->numHops, rxCmd->packetsPerHop);
        bool hopping = (genericHandlerState.stream.numHops != 0U);

        /* FIFOCFG and EXTRABYTES as the settings left them, taken before
         * anything can fail: the end of the command puts them back however
         * it ended, and a command refused at setup must put back what it
         * found, not what an earlier one kept. */
        genericHandlerState.stream.fifoCfg = (uint16_t) HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_FIFOCFG);
        genericHandlerState.stream.extraBytes = (uint16_t) HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_EXTRABYTES);
        /* The timeouts of a hopping command: a packet one period late ends
         * the operation, and an operation that hears nothing ends 1.25
         * dwells after its post, see the command's description */
        uint32_t rxTimeout = rxCmd->packetPeriodTicks;
        uint32_t firstRxTimeout = ((uint32_t) rxCmd->packetsPerHop * rxTimeout * 5U) / 4U;

        if (hopping && ((rxTimeout == 0U) || (firstRxTimeout < 512U) || (firstRxTimeout > 0xFFFFU)))
        {
            /* The timeout is 16 bits and, per the register description, at
             * least 128 us when set */
            hopOk = false;
        }
        if (hopping && ((cmd->timing.relHardStopTime != 0U) || (cmd->timing.relGracefulStopTime != 0)))
        {
            /* A stop time reaches the PBE through the SysTimer compares
             * and not through the handler, and a stop written into a
             * running operation with the synthesizer kept on is the wedge
             * the deferred stop exists to avoid; see the command's
             * description */
            hopOk = false;
        }

        /* Start by enabling refsys */
        earliestStartTime = RCL_Handler_Generic_prepareSynth();

        if ((frequency == 0U) && (LRF_Interface_Generic_isFreqSynthLocked() == false))
        {
            /* Synth not to be programmed, but not already locked */
            cmd->status = RCL_CommandStatus_Error_Synth;
            rclEvents.lastCmdDone = 1U;
        }
        else if (!hopOk)
        {
            cmd->status = RCL_CommandStatus_Error_Param;
            rclEvents.lastCmdDone = 1U;
        }
        else
        {
            /* Mark as active */
            cmd->status = RCL_CommandStatus_Active;
            genericHandlerState.stream.stopType = RCL_StopType_None;

            /* Program the sync word */
            LRF_Interface_Generic_programSyncWordA(rxCmd->syncWord);

            /* A repeated RX operation: the PBE goes back to sync search
             * after each packet and receives whatever arrives, for as long
             * as the command runs. OPCFG.TXFCMD is left at NONE by this, so
             * the PBE issues no TX FIFO command at the end of a packet.
             *
             * Without a hop table there is one operation with no timeouts,
             * and it turns the synthesizer off when it ends; config.fsOff
             * only decides below whether refsys and the power constraints
             * are kept for a following command. Nothing is kept on for, and
             * a stop in sync search relies on it: the PBE's end routine
             * waits for the RFE to report, the RFE reports its RX command
             * only once told to stop, and it is told to stop only when the
             * synthesizer is not kept on. With it kept on, a stop that
             * lands in sync search leaves the PBE waiting forever; measured.
             *
             * With a hop table there is an operation per dwell and the
             * synthesizer is kept on from the first, so that the later
             * ones, posted without a calibration, find it running: the RFE
             * does not start a synthesizer that has been turned off for an
             * operation without a calibration. A stop is therefore never
             * written into a running operation of a hopping command, see
             * below; the timeouts bound every operation instead. */
            LRF_Interface_Generic_configOpRx(hopping ? 0U : 1U, frequency, 1U, rxCmd->packetLength);
            HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_SEQSTAT0) = PBE_GENERIC_RAM_SEQSTAT0_STOPAUTO_NEVER;
            if (hopping)
            {
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_RXTIMEOUT) = (uint16_t) rxTimeout;
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_FIRSTRXTIMEOUT) = (uint16_t) firstRxTimeout;
            }

            /* Nothing is appended to an entry. The RF settings append status,
             * RSSI and timestamp bytes; with them off an entry is the length
             * field, the pad and the payload, which is what the application
             * arms one transfer for. The settings' values, kept above, are
             * put back when the command ends: they are only reloaded when
             * the radio is set up again, and the next RX command on the
             * same configuration counts on them. */
            uint16_t fifoCfg = genericHandlerState.stream.fifoCfg;

            fifoCfg &= (uint16_t) ~(PBE_GENERIC_RAM_FIFOCFG_APPENDCRC_M |
                                    PBE_GENERIC_RAM_FIFOCFG_APPENDSTATUS_M |
                                    PBE_GENERIC_RAM_FIFOCFG_APPENDLQI_M |
                                    PBE_GENERIC_RAM_FIFOCFG_APPENDFREQEST_M |
                                    PBE_GENERIC_RAM_FIFOCFG_APPENDRSSI_M |
                                    PBE_GENERIC_RAM_FIFOCFG_APPENDTIMESTAMP_M);
            HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_FIFOCFG) = fifoCfg;
            HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_EXTRABYTES) = 0U;

            /* Reset the received-packet counters, reported through %stats.
             * NRXTARGET is zeroed as well: this PBE image ends an operation
             * when the count reaches it, the word lies outside the settings
             * image, and a stream is not to end on a count. */
            LRF_Interface_Generic_setNumOfRxOkPackets(0U);
            LRF_Interface_Generic_setNumOfNotRxOkPackets(0U);
            HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXTARGET) = 0U;

            /* Program the frequency, or every channel of the hop table */
            rclGenericStreamProgramFrequency(frequency, rxCmd->hopFrequencies, false);

            /* Enable radio */
            if (rxCmd->config.enableLRF != 0U)
            {
                LRF_enable();
            }

            /* Reset the FIFO, then work out the footprint of one entry as
             * the PBE will write it: the numPad field (1), the optional
             * padding and header, the payload and whatever is appended, now
             * nothing, plus the 2-byte length field, rounded up to a word.
             * The envelope inputs come from the settings-programmed
             * registers, so nothing here is PHY-specific. */
            genericHandlerState.common.rxFifoSize = (uint16_t) LRF_prepareRxFifo();

            uint16_t rxOpCfg = (uint16_t) HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_OPCFG);
            uint32_t rxOptPad = ((uint32_t) fifoCfg & PBE_GENERIC_RAM_FIFOCFG_LENOPTPAD_M) >> PBE_GENERIC_RAM_FIFOCFG_LENOPTPAD_S;
            uint32_t rxHdrBytes = 0U;

            if ((rxOpCfg & PBE_GENERIC_RAM_OPCFG_RXINCLUDEHDR_M) != 0U)
            {
                uint16_t rxPktCfg = (uint16_t) HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_PKTCFG);
                uint32_t rxNumHdrBits = ((uint32_t) rxPktCfg & PBE_GENERIC_RAM_PKTCFG_NUMHDRBITS_M) >> PBE_GENERIC_RAM_PKTCFG_NUMHDRBITS_S;
                rxHdrBytes = (rxNumHdrBits + 7U) / 8U;
            }
            rxCmd->entryBytes = (uint16_t) RCL_Buffer_DataEntry_paddedLen(1U + rxOptPad + rxHdrBytes + (uint32_t) rxCmd->packetLength);

            /* Give the PBE the whole FIFO with one pointer write now, while
             * it is not running, and let auto deallocate keep the space
             * open from then on: reading through the data port moves RXFRP
             * but not RXFSRP, and FCFG0.RXADEAL moves RXFSRP after it. No
             * FIFO pointer is written while a packet can be on the air. */
            LRF_setRxFifoEffSz(genericHandlerState.common.rxFifoSize);
            HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0) =
                HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0) | LRFDPBE_FCFG0_RXADEAL_M;

            /* One DMA request per committed entry, for the application's
             * channel */
            RCL_Dma_enableRxCommitTrigger();

            RCL_CommandStatus startTimeStatus = RCL_Scheduler_setStartStopTimeEarliestStart(cmd, earliestStartTime);
            if (startTimeStatus >= RCL_CommandStatus_Finished)
            {
                cmd->status = startTimeStatus;
                rclEvents.lastCmdDone = 1U;
            }
            else
            {
                /* Only an operation error is of interest; the CPU is idle
                 * between packets. With a hop table the operation done is
                 * too: it comes once per dwell and is where the hop is
                 * made. A stop of a hopping command is answered at that
                 * end as well, and RCL is told to write none to the PBE. */
                if (hopping)
                {
                    LRF_enableHwInterrupt(LRF_EventOpDone.value | LRF_EventOpError.value);
                    rclSchedulerState.handlerStops = 1U;
                }
                else
                {
                    LRF_enableHwInterrupt(LRF_EventOpError.value);
                }

                Log_printf(LogModule_RCL, Log_INFO, "RCL_Handler_Generic_RxStream: Starting RX (%u Hz)", frequency);
                LRF_waitForTopsmReady();
                RCL_Profiling_eventHook(RCL_ProfilingEvent_PreprocStop);
                LRF_Interface_Generic_sendOpRx();
            }
        }
    }

    if (cmd->status == RCL_CommandStatus_Active)
    {
        if (rclEventsIn.timerStart != 0U)
        {
            rclGenericStreamEnableStopTimeIrqs();
            rclEvents.cmdStarted = 1U;
        }

        if ((lrfEvents.opDone != 0U) || (lrfEvents.opError != 0U))
        {
            if ((genericHandlerState.stream.numHops != 0U) && rclGenericRxStreamContinue())
            {
                /* The next operation of the hopping command is posted */
            }
            else
            {
                cmd->status = rclGenericStreamEndStatus(lrfEvents);
                rclEvents.lastCmdDone = 1U;
                RCL_Profiling_eventHook(RCL_ProfilingEvent_PostprocStart);
            }
        }
        else if ((rclEventsIn.gracefulStop != 0U) || (rclEventsIn.hardStop != 0U))
        {
            if (genericHandlerState.stream.numHops != 0U)
            {
                /* Honoured when the operation ends, which is brought about
                 * here rather than waited for: with packets arriving and no
                 * count armed the operation would run on forever. The count
                 * is armed so that the next packet ends it, and if no packet
                 * comes the timeouts do. Not posting again is then what ends
                 * the command. */
                rclGenericStreamNoteStop(rclEventsIn);
                HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXTARGET) =
                    (uint16_t) (HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXOK) +
                                HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXNOK) + 1U);
            }
            else
            {
                rclGenericStreamRequestStop(rclEventsIn);
            }
        }
        else
        {
            /* Other events */
        }
    }

    if (rclEvents.lastCmdDone != 0U)
    {
        /* The trigger has to go with the command: once the FIFO is empty
         * the selected condition holds again and the request would stay
         * asserted with nothing to serve. The channel is the
         * application's and is left alone. */
        RCL_Dma_disableTrigger();

        /* The entry envelope as the settings define it, for the commands
         * that follow */
        HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_FIFOCFG) = genericHandlerState.stream.fifoCfg;
        HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_EXTRABYTES) = genericHandlerState.stream.extraBytes;

        if (rxCmd->stats != NULL)
        {
            /* The last operation's counts on top of the ended ones' */
            uint32_t nRxOk = genericHandlerState.stream.rxOk + HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXOK);
            uint32_t nRxNok = genericHandlerState.stream.rxNok + HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_NRXNOK);

            if (rxCmd->stats->config.accumulate == 0U)
            {
                rxCmd->stats->nRxOk = (uint16_t) nRxOk;
                rxCmd->stats->nRxNok = (uint16_t) nRxNok;
                rxCmd->stats->nHops = (uint16_t) genericHandlerState.stream.hops;
            }
            else
            {
                rxCmd->stats->nRxOk += (uint16_t) nRxOk;
                rxCmd->stats->nRxNok += (uint16_t) nRxNok;
                rxCmd->stats->nHops += (uint16_t) genericHandlerState.stream.hops;
            }
        }

        if (rxCmd->config.disableLRF != 0U)
        {
            LRF_disable();
        }
        RCL_Handler_Generic_setSynthPowerState((bool) rxCmd->config.fsOff);
    }

    return rclEvents;
}

#else /* No LRF DMA: the burst and stream commands are not available */

RCL_Events RCL_Handler_Generic_TxBurst(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_Events rclEvents = {.value = 0U};
    (void) lrfEvents;

    if (rclEventsIn.setup != 0U)
    {
        cmd->status = RCL_CommandStatus_Error_Param;
        rclEvents.lastCmdDone = 1U;
    }

    return rclEvents;
}

RCL_Events RCL_Handler_Generic_RxBurst(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_Events rclEvents = {.value = 0U};
    (void) lrfEvents;

    if (rclEventsIn.setup != 0U)
    {
        cmd->status = RCL_CommandStatus_Error_Param;
        rclEvents.lastCmdDone = 1U;
    }

    return rclEvents;
}

RCL_Events RCL_Handler_Generic_TxStream(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_Events rclEvents = {.value = 0U};
    (void) lrfEvents;

    if (rclEventsIn.setup != 0U)
    {
        cmd->status = RCL_CommandStatus_Error_Param;
        rclEvents.lastCmdDone = 1U;
    }

    return rclEvents;
}

RCL_Events RCL_Handler_Generic_RxStream(RCL_Command *cmd, LRF_Events lrfEvents, RCL_Events rclEventsIn)
{
    RCL_Events rclEvents = {.value = 0U};
    (void) lrfEvents;

    if (rclEventsIn.setup != 0U)
    {
        cmd->status = RCL_CommandStatus_Error_Param;
        rclEvents.lastCmdDone = 1U;
    }

    return rclEvents;
}

void RCL_CmdGenericRxStream_hopSync(RCL_CmdGenericRxStream *cmd, uint32_t packetCounter)
{
    (void) cmd;
    (void) packetCounter;
}

#endif /* DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX */

/*
 *  ======== RCL_Handler_Generic_prepareSynth ========
 */
static uint32_t RCL_Handler_Generic_prepareSynth(void)
{
    /* Power up synth refsys and set a constraint on swtcxo to ensure it is not changed while radio is running */
    if (!genericHandlerState.common.powerSwtcxoConstraintSet)
    {
        genericHandlerState.common.powerSwtcxoConstraintSet = true;
        RCL_Hal_powerSetSwTcxoUpdateConstraint();
    }
    return LRF_enableSynthRefsys();
}

/*
 *  ======== RCL_Handler_Generic_setSynthPowerState ========
 */
static void RCL_Handler_Generic_setSynthPowerState(bool fsOff)
{
    /* Do power management for synth at the end of a command.
       If synth is off, turn off refsys and remove constraint on standby and swtcxo.
       If synth is on, keep refsys on and ensure constraint on standby is set */
    if (fsOff)
    {
        LRF_disableSynthRefsys();
        /* Release additional power standby constraints if necessary */
        if (genericHandlerState.common.powerStandbyConstraintSet)
        {
            genericHandlerState.common.powerStandbyConstraintSet = false;
            RCL_Hal_powerReleaseStandbyConstraint();
        }
        /* Release power SWTCXO constraints if necessary */
        if (genericHandlerState.common.powerSwtcxoConstraintSet)
        {
            genericHandlerState.common.powerSwtcxoConstraintSet = false;
            RCL_Hal_powerReleaseSwTcxoUpdateConstraint();
        }
    }
    else
    {
        /* Set additional power constraints if necessary */
        if (!genericHandlerState.common.powerStandbyConstraintSet)
        {
            genericHandlerState.common.powerStandbyConstraintSet = true;
            RCL_Hal_powerSetStandbyConstraint();
        }
    }
}

/*
 *  ======== RCL_Handler_Generic_updateRxCurBufferAndFifo ========
 */
static void RCL_Handler_Generic_updateRxCurBufferAndFifo(List_List *rxBuffers)
{
    RCL_MultiBuffer *curBuffer = genericHandlerState.common.curBuffer;

    if (curBuffer == NULL)
    {
        curBuffer = RCL_MultiBuffer_findFirstWritableBuffer((RCL_MultiBuffer *)rxBuffers->head);
    }
    genericHandlerState.common.curBuffer = curBuffer;

    uint32_t rxSpace = RCL_MultiBuffer_findAvailableRxSpace(curBuffer);

    LRF_setRxFifoEffSz(rxSpace);
}

/*
 *  ======== RCL_Handler_Generic_mapLrfErrorStatusToRclStatus ========
 */
static RCL_CommandStatus RCL_Handler_Generic_mapLrfErrorStatusToRclStatus(void)
{
    /* Get LRF command end cause */
    uint16_t lrfCmdEndCause = LRF_Interface_getCmdEndCause();

    /* Map LRF command end cause to corresponding RCL command status */
    RCL_CommandStatus status;
    switch (lrfCmdEndCause)
    {
    case LRF_INTERFACE_ENDCAUSE_STAT_ERR_RXF:
        status = RCL_CommandStatus_Error_RxFifo;
        break;
    case LRF_INTERFACE_ENDCAUSE_STAT_ERR_TXF:
        status = RCL_CommandStatus_Error_TxFifo;
        break;
    case LRF_INTERFACE_ENDCAUSE_STAT_ERR_SYNTH:
        status = RCL_CommandStatus_Error_Synth;
        break;
    case LRF_INTERFACE_ENDCAUSE_STAT_RXTIMEOUT:
        status = RCL_CommandStatus_RxTimeout;
        break;
    case LRF_INTERFACE_ENDCAUSE_STAT_EOPSTOP:
        status = RCL_Scheduler_findStopStatus(RCL_StopType_Graceful);
        break;
    case LRF_INTERFACE_ENDCAUSE_STAT_ERR_STOP:
        status = RCL_Scheduler_findStopStatus(RCL_StopType_Hard);
        break;
    case LRF_INTERFACE_ENDCAUSE_STAT_ERR_BADOP:
        status = RCL_CommandStatus_Error_UnknownOp;
        break;
    default:
        Log_printf(LogModule_RCL, Log_ERROR, "RCL_Handler_Generic_mapLrfErrorStatusToRclStatus: Unexpected error 0x%04X from LRF", lrfCmdEndCause);
        status = RCL_CommandStatus_Error;
        break;
    }

    return status;
}

/*
 *  ======== RCL_Handler_Generic_updateTxBuffers ========
 */
static uint32_t RCL_Handler_Generic_updateTxBuffers(List_List *txBuffers,
                                                    uint32_t maxBuffers)
{
    uint32_t nBuffers = 0;
    RCL_Buffer_TxBuffer *nextTxBuffer;

    nextTxBuffer = RCL_TxBuffer_head(txBuffers);

    while (nextTxBuffer != NULL && nBuffers < maxBuffers)
    {
        uint32_t length = nextTxBuffer->length;
        /* Number of words including length field and end padding */
        uint32_t wordLength = RCL_Buffer_DataEntry_paddedLen(length) / 4U;

        if (wordLength > LRF_getTxFifoWritable() / 4U)
        {
            /* Packet will not fit */
            /* TODO: See RCL-348 */
            break;
        }
        nextTxBuffer->state = RCL_BufferStateInUse;
        uint32_t *data32 = (uint32_t *) &(nextTxBuffer->length);

        /* Copy packet into FIFO */
        LRF_writeTxFifoWords(data32, wordLength);
        nextTxBuffer = RCL_TxBuffer_next(nextTxBuffer);

        nBuffers++;
    }

    return nBuffers;
}

/*
 *  ======== RCL_Handler_Generic_updateRxStats ========
 */
static void RCL_Handler_Generic_updateRxStats(RCL_StatsGeneric *stats, uint32_t startTime)
{
    if (stats != NULL)
    {
        uint32_t lastTimestamp = LRF_Interface_Generic_getLastPacketTimestamp();
        /* Check if a new value is found in the first timestamp */
        if (lastTimestamp == startTime)
        {
            stats->timestampValid = 0U;
        }
        else {
            stats->timestampValid = 1U;
            stats->lastTimestamp = lastTimestamp;
        }
        stats->lastRssi = LRF_Interface_Generic_getLastPacketRssi();
        RCL_Handler_Generic_updateLongStats();
        stats->nRxNok = genericHandlerState.rx.longNokCount;
        stats->nRxOk = genericHandlerState.rx.longOkCount;
    }
}

/*
 *  ======== RCL_Handler_Generic_updateLongStats ========
 */
static void RCL_Handler_Generic_updateLongStats(void)
{
    uint32_t oldRxOk = genericHandlerState.rx.longOkCount;
    uint32_t oldRxNok = genericHandlerState.rx.longNokCount;
    uint32_t newRxOk = (oldRxOk & ~0xFFFFU) | LRF_Interface_Generic_getNumOfRxOkPackets();
    uint32_t newRxNok = (oldRxNok & ~0xFFFFU) | LRF_Interface_Generic_getNumOfNotRxOkPackets();

    if (newRxOk < oldRxOk)
    {
        newRxOk += 0x10000U;
    }
    if (newRxNok < oldRxNok)
    {
        newRxNok += 0x10000U;
    }
    genericHandlerState.rx.longOkCount = newRxOk;
    genericHandlerState.rx.longNokCount = newRxNok;
}

/*
 *  ======== RCL_Handler_Generic_initRxStats ========
 */
static bool RCL_Handler_Generic_initRxStats(RCL_StatsGeneric *stats, uint32_t startTime)
{
    if (stats != NULL)
    {
        /* Set timestamp to start time of command (will not occur again) to know if a valid value has been found */
        LRF_Interface_Generic_setLastPacketTimestamp(startTime);
        stats->timestampValid = 0U;
        stats->lastRssi = LRF_RSSI_INVALID;
        if (stats->config.accumulate != 0U)
        {
            /* Copy existing values into LRF */
            genericHandlerState.rx.longNokCount = stats->nRxNok;
            LRF_Interface_Generic_setNumOfNotRxOkPackets((uint16_t) stats->nRxNok & 0xFFFFU);
            genericHandlerState.rx.longOkCount = stats->nRxOk;
            LRF_Interface_Generic_setNumOfRxOkPackets((uint16_t) stats->nRxOk & 0xFFFFU);
        }
        else
        {
            /* Reset existing values in LRF */
            genericHandlerState.rx.longNokCount = 0U;
            LRF_Interface_Generic_setNumOfNotRxOkPackets(0U);
            genericHandlerState.rx.longOkCount = 0U;
            LRF_Interface_Generic_setNumOfRxOkPackets(0U);

            stats->nRxNok = 0U;
            stats->nRxOk = 0U;
        }
        return (bool) stats->config.activeUpdate;
    }
    else
    {
        /* Reset existing values in LRF */
        genericHandlerState.rx.longNokCount = 0U;
        LRF_Interface_Generic_setNumOfNotRxOkPackets(0U);
        genericHandlerState.rx.longOkCount = 0U;
        LRF_Interface_Generic_setNumOfRxOkPackets(0U);

        return false;
    }
}

/*
 *  ======== RCL_Handler_Nesb_updateHeader ========
 */
static void RCL_Handler_Nesb_updateHeader(List_List *txBuffers, uint8_t autoRetransmitMode,
                                          uint8_t hdrConf, uint8_t seqNumber)
{
    uint8_t noAck;
    uint8_t seqNo;

    RCL_Buffer_TxBuffer *nextTxBuffer;
    nextTxBuffer = RCL_TxBuffer_head(txBuffers);
    uint8_t indexHeader = nextTxBuffer->numPad - 1U;

    if (hdrConf == 0U)
    {
        /* Insert NO_ACK field from TX buffer. */
        noAck = nextTxBuffer->data[indexHeader] & 0x01U;
        seqNo = seqNumber;
    }
    else
    {
        /* Insert SEQ and NO_ACK field from TX buffer. */
        noAck = nextTxBuffer->data[indexHeader] & 0x01U;
        seqNo = (nextTxBuffer->data[indexHeader] >> 1) & 0x03U;
    }

    /* Update header */
    nextTxBuffer->data[indexHeader] = ((nextTxBuffer->data[indexHeader] & 0xF8U) | ((seqNo & 0x03U) << 1) | noAck);
}

/*
 *  ======== RCL_Handler_Nesb_updateStats ========
 */
static void RCL_Handler_Nesb_updateStats(RCL_StatsNesb *stats, uint32_t startTime)
{
    if (stats != NULL)
    {
        uint32_t lastTimestamp = LRF_Interface_Generic_getLastPacketTimestamp();
        /* Check if a new value is found in the first timestamp */
        if (lastTimestamp == startTime)
        {
            stats->timestampValid = 0U;
        }
        else {
            stats->timestampValid = 1U;
            stats->lastTimestamp = lastTimestamp;
        }
        stats->lastRssi = LRF_Interface_Generic_getLastPacketRssi();
        RCL_Handler_Nesb_updateLongStats();
        stats->nTx = genericHandlerState.nesb.longTxCount;
        stats->nRxOk = genericHandlerState.nesb.longOkCount;
        stats->nRxNok = genericHandlerState.nesb.longNokCount;
        stats->nRxIgnored = genericHandlerState.nesb.longRxIgnoredCount;
        stats->nRxAddrMismatch = genericHandlerState.nesb.longRxAddrMismatchCount;
        stats->nRxBufFull = genericHandlerState.nesb.longRxBufFullCount;
    }
}

/*
 *  ======== RCL_Handler_Nesb_updateLongStats ========
 */
static void RCL_Handler_Nesb_updateLongStats(void)
{
    uint32_t oldTx = genericHandlerState.nesb.longTxCount;
    uint32_t oldRxOk = genericHandlerState.nesb.longOkCount;
    uint32_t oldRxNok = genericHandlerState.nesb.longNokCount;
    /* TODO: RCL-308: Long counters should not be needed for anything except RX Ok and CRC error */
    uint32_t oldRxIgnored = genericHandlerState.nesb.longRxIgnoredCount;
    uint32_t oldRxAddrMismatch = genericHandlerState.nesb.longRxAddrMismatchCount;
    uint32_t oldRxBufFull = genericHandlerState.nesb.longRxBufFullCount;

    uint32_t newTx = (oldTx & ~0xFFFFU) | LRF_Interface_Generic_getNumOfTxPackets();
    uint32_t newRxOk = (oldRxOk & ~0xFFFFU) | LRF_Interface_Generic_getNumOfRxOkPackets();
    uint32_t newRxNok = (oldRxNok & ~0xFFFFU) | LRF_Interface_Generic_getNumOfNotRxOkPackets();
    uint32_t newRxIgnored = (oldRxIgnored & ~0xFFFFU) | LRF_Interface_Generic_getNumOfIgnoredRxPackets();
    /* TODO: See RCL-343 */
    uint32_t newRxAddrMismatch = (oldRxAddrMismatch & ~0xFFFFU) | LRF_Interface_Generic_getNumOfIgnoredRxPackets();
    uint32_t newRxBufFull = (oldRxBufFull & ~0xFFFFU) | LRF_Interface_Generic_getRxFifoFullCount();

    if (newTx < oldTx)
    {
        newTx += 0x10000U;
    }
    if (newRxOk < oldRxOk)
    {
        newRxOk += 0x10000U;
    }
    if (newRxNok < oldRxNok)
    {
        newRxNok += 0x10000U;
    }
    if (newRxIgnored < oldRxIgnored)
    {
        newRxIgnored += 0x10000U;
    }
    if (newRxAddrMismatch < oldRxAddrMismatch)
    {
        newRxAddrMismatch += 0x10000U;
    }
    if (newRxBufFull < oldRxBufFull)
    {
        newRxBufFull += 0x10000U;
    }
    genericHandlerState.nesb.longTxCount = newTx;
    genericHandlerState.nesb.longOkCount = newRxOk;
    genericHandlerState.nesb.longNokCount = newRxNok;
    genericHandlerState.nesb.longRxIgnoredCount = newRxIgnored;
    genericHandlerState.nesb.longRxAddrMismatchCount = newRxAddrMismatch;
    genericHandlerState.nesb.longRxBufFullCount = newRxBufFull;
}

/*
 *  ======== RCL_Handler_Nesb_initStats ========
 */
static bool RCL_Handler_Nesb_initStats(RCL_StatsNesb *stats, uint32_t startTime)
{
    if (stats != NULL)
    {
        /* Set timestamp to start time of command (will not occur again) to know if a valid value has been found */
        LRF_Interface_Generic_setLastPacketTimestamp(startTime);
        stats->timestampValid = 0U;
        stats->lastRssi = LRF_RSSI_INVALID;
        if (stats->config.accumulate != 0U)
        {
            /* Copy existing values into LRF */
            genericHandlerState.nesb.longTxCount = stats->nTx;
            LRF_Interface_Generic_setNumOfTxPackets((uint16_t) stats->nRxOk & 0xFFFFU);
            genericHandlerState.nesb.longOkCount = stats->nRxOk;
            LRF_Interface_Generic_setNumOfRxOkPackets((uint16_t) stats->nRxOk & 0xFFFFU);
            genericHandlerState.nesb.longNokCount = stats->nRxNok;
            LRF_Interface_Generic_setNumOfNotRxOkPackets((uint16_t) stats->nRxNok & 0xFFFFU);
            genericHandlerState.nesb.longRxIgnoredCount = stats->nRxIgnored;
            LRF_Interface_Generic_setNumOfIgnoredRxPackets((uint16_t) stats->nRxIgnored & 0xFFFFU);
            genericHandlerState.nesb.longRxAddrMismatchCount = stats->nRxAddrMismatch;
            LRF_Interface_Generic_setNumOfIgnoredRxPackets((uint16_t) stats->nRxAddrMismatch & 0xFFFFU);
            genericHandlerState.nesb.longRxBufFullCount = stats->nRxBufFull;
            LRF_Interface_Generic_setRxFifoFullCount((uint16_t) stats->nRxBufFull & 0xFFFFU);
        }
        else
        {
            /* Reset existing values in LRF */
            genericHandlerState.nesb.longTxCount = 0U;
            LRF_Interface_Generic_setNumOfTxPackets(0U);
            genericHandlerState.nesb.longOkCount = 0U;
            LRF_Interface_Generic_setNumOfRxOkPackets(0U);
            genericHandlerState.nesb.longNokCount = 0U;
            LRF_Interface_Generic_setNumOfNotRxOkPackets(0U);
            genericHandlerState.nesb.longRxIgnoredCount = 0U;
            LRF_Interface_Generic_setNumOfIgnoredRxPackets(0U);
            genericHandlerState.nesb.longRxAddrMismatchCount = 0U;
            LRF_Interface_Generic_setNumOfIgnoredRxPackets(0U);
            genericHandlerState.nesb.longRxBufFullCount = 0U;
            LRF_Interface_Generic_setRxFifoFullCount(0U);

            stats->nTx = 0U;
            stats->nRxOk = 0U;
            stats->nRxNok = 0U;
            stats->nRxIgnored = 0U;
            stats->nRxAddrMismatch = 0U;
            stats->nRxBufFull = 0U;
        }
        return (bool) stats->config.activeUpdate;
    }
    else
    {
        /* Reset existing values in LRF */
        genericHandlerState.nesb.longTxCount = 0U;
        LRF_Interface_Generic_setNumOfTxPackets(0U);
        genericHandlerState.nesb.longOkCount = 0U;
        LRF_Interface_Generic_setNumOfRxOkPackets(0U);
        genericHandlerState.nesb.longNokCount = 0U;
        LRF_Interface_Generic_setNumOfNotRxOkPackets(0U);
        genericHandlerState.nesb.longRxIgnoredCount = 0U;
        LRF_Interface_Generic_setNumOfIgnoredRxPackets(0U);
        genericHandlerState.nesb.longRxAddrMismatchCount = 0U;
        LRF_Interface_Generic_setNumOfIgnoredRxPackets(0U);
        genericHandlerState.nesb.longRxBufFullCount = 0U;
        LRF_Interface_Generic_setRxFifoFullCount(0U);

        return false;
    }
}
