/*
 * Copyright (c) 2024 Texas Instruments Incorporated - http://www.ti.com
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
 *  ======== LogSinkTraceLPF3.c ========
 */

#include <stdint.h>
#include <stdarg.h>

#include <ti/log/Log.h>
#include <ti/log/LogSinkTraceLPF3.h>
#include <ti/drivers/Power.h>
#include <ti/drivers/GPIO.h>

#include <ti/drivers/dpl/HwiP.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_lrfdtrc.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(driverlib/hapi.h)

/* The GPO mux value to select the RFCTRC signal. It is assumed to be the same for all GPOs. */
#define GPO_RFCTRC_MUX_VALUE (LRFDDBELL_GPOSEL0_SRC0_RFCTRC >> LRFDDBELL_GPOSEL0_SRC0_S)

/* Check that assumption regarding mux value for the RFCTRC signal holds up */
#if (LRFDDBELL_GPOSEL1_SRC7_RFCTRC >> LRFDDBELL_GPOSEL1_SRC7_S) != GPO_RFCTRC_MUX_VALUE || \
    (LRFDDBELL_GPOSEL1_SRC6_RFCTRC >> LRFDDBELL_GPOSEL1_SRC6_S) != GPO_RFCTRC_MUX_VALUE || \
    (LRFDDBELL_GPOSEL1_SRC5_RFCTRC >> LRFDDBELL_GPOSEL1_SRC5_S) != GPO_RFCTRC_MUX_VALUE || \
    (LRFDDBELL_GPOSEL1_SRC4_RFCTRC >> LRFDDBELL_GPOSEL1_SRC4_S) != GPO_RFCTRC_MUX_VALUE || \
    (LRFDDBELL_GPOSEL0_SRC3_RFCTRC >> LRFDDBELL_GPOSEL0_SRC3_S) != GPO_RFCTRC_MUX_VALUE || \
    (LRFDDBELL_GPOSEL0_SRC2_RFCTRC >> LRFDDBELL_GPOSEL0_SRC2_S) != GPO_RFCTRC_MUX_VALUE || \
    (LRFDDBELL_GPOSEL0_SRC1_RFCTRC >> LRFDDBELL_GPOSEL0_SRC1_S) != GPO_RFCTRC_MUX_VALUE
    #error "Assumption is wrong. RFCTRC mux value is not consistent between GPOs"
#endif

/* Bit field mask and shift helper for Tracer channel */
#define LogSinkTraceLPF3_CHANNEL_M 0x07000000U
#define LogSinkTraceLPF3_CHANNEL_S 24

/* Bit field mask and shift helper for log ID */
#define LogSinkTraceLPF3_HEADER_PTR_OFFSET 4
#define LogSinkTraceLPF3_LOGID_S           6
#define HEADERPTR_LOGID_M                  0x000003FCU

#define MAX_ARG_COUNT     4
#define CHANNEL_FIELD_MAX 4

/* Runtime level filter: emit only when the level is enabled, either through a
 * module's dynamic (runtime-settable) bitmap or its constant one. */
#define LogSinkTraceLPF3_LEVEL_ENABLED(handle, level) \
    (((handle->dynamicLevelsPtr != NULL) && (level & *(handle->dynamicLevelsPtr))) || (handle->levels & level))

/* Look up table to get comnbined length of arguments
 * 1 and 2 arguments have double length */
static const uint8_t nArgLut[MAX_ARG_COUNT + 1] = {0, 2, 4, 3, 4};

/* Look up table to get channel ind */
static const uint8_t channelLut[CHANNEL_FIELD_MAX + 1] = {0, 0, 1, 2, 0};

static Power_NotifyObj LogSinkTraceLPF3_powerAwakeStandbyObj;

static int_fast16_t LogSinkTraceLPF3_postNotify(uint_fast16_t eventType, uintptr_t eventArg, uintptr_t clientArg);

void LogSinkTraceLPF3_enable(void)
{
    /* Enable the tracer on target */
    HWREG_WRITE_LRF(LRFDTRC_BASE + LRFDTRC_O_CFG) = LogSinkTraceLPF3_config.tracerConfig;

    /* Set tracer pin */
    uint32_t lrfdTracerPin = LogSinkTraceLPF3_config.lrfdTracerPin;
    uint32_t gpoCfg = HWREG_READ_LRF(LRFDDBELL_BASE + LRFDDBELL_O_GPOSEL0 + (lrfdTracerPin / 4) * sizeof(uint32_t));
    gpoCfg &= ~((LRFDDBELL_GPOSEL0_SRC0_M >> LRFDDBELL_GPOSEL0_SRC0_S) << ((lrfdTracerPin % 4) * 8));
    gpoCfg |= GPO_RFCTRC_MUX_VALUE << ((lrfdTracerPin % 4) * 8);
    HWREG_WRITE_LRF(LRFDDBELL_BASE + LRFDDBELL_O_GPOSEL0 + (lrfdTracerPin / 4) * sizeof(uint32_t)) = gpoCfg;

    /* Give enough time for FPGA tracer to lock on to signal */
    HapiWaitUs(5);
}

void LogSinkTraceLPF3_init(void)
{
    /* Enable tracer clock */
    Power_setDependency(PowerLPF3_PERIPH_LFRD_TRC);

    /* Register for wakeup event */
    Power_registerNotify(&LogSinkTraceLPF3_powerAwakeStandbyObj,
                         PowerLPF3_AWAKE_STANDBY,
                         (Power_NotifyFxn)LogSinkTraceLPF3_postNotify,
                         (uintptr_t)NULL);

    GPIO_setConfigAndMux(LogSinkTraceLPF3_config.tracerPin, GPIO_CFG_NO_DIR, LogSinkTraceLPF3_config.tracerPinMux);

    LogSinkTraceLPF3_enable();
}

/*
 *  ======== LogSinkTraceLPF3_send ========
 *  Shared emit path. The tracer hardware takes at most two 32-bit parameter
 *  words per packet, so callers pass the arguments already packed into
 *  arg01/arg23 and this helper only has to program the channel registers.
 *  Callers guarantee numArgs <= MAX_ARG_COUNT.
 */
static void LogSinkTraceLPF3_send(uint32_t headerPtr, uint32_t numArgs, uint32_t arg01, uint32_t arg23)
{
    uint32_t tracerCommand;
    int32_t channelField;
    int32_t channelIndex;
    uint32_t key;

    /* Check the channel field to see which channel to send the log to */
    channelField = ((headerPtr & LogSinkTraceLPF3_CHANNEL_M) >> LogSinkTraceLPF3_CHANNEL_S);

    if (channelField > CHANNEL_FIELD_MAX)
    {
        channelField = CHANNEL_FIELD_MAX;
    }

    /* Convert channel field to the correct index */
    channelIndex = channelLut[channelField];

    /* Extract the 8-bit ID, which gives 256 log statements for each channel.
     * IDs 1 and 2 are reserved for legacy purposes in the tracer GUI and may
     * not be allocated by an application.
     *
     * Starting adress (headerPtr) for logs is 0x94000008 in the outfile. This
     * corresponds to ID 2. The header address needs to be increased by offset 4 so
     * that the first ID becomes ID 3.
     *
     * The extracted ID is combined with the number of arguments and passed to
     * the tracer hardware as a tracer command
     */
    tracerCommand = (((headerPtr + LogSinkTraceLPF3_HEADER_PTR_OFFSET) & HEADERPTR_LOGID_M)
                     << LogSinkTraceLPF3_LOGID_S) |
                    nArgLut[numArgs];

    /* Disable interrupts */
    key = HwiP_disable();

    /* Wait until tracer channel is available */
    while ((HWREG_READ_LRF(LRFDTRC_BASE + LRFDTRC_O_CH1CMD + sizeof(uint32_t) * channelIndex) &
            LRFDTRC_CH1CMD_PKTHDR_M) != 0)
    {}

    if (numArgs >= 1)
    {
        HWREG_WRITE_LRF(LRFDTRC_BASE + LRFDTRC_O_CH1PAR01 + sizeof(uint32_t) * channelIndex) = arg01;
    }
    if (numArgs >= 2)
    {
        HWREG_WRITE_LRF(LRFDTRC_BASE + LRFDTRC_O_CH1PAR23 + sizeof(uint32_t) * channelIndex) = arg23;
    }

    /* Channel ready, transmit packet */
    HWREG_WRITE_LRF(LRFDTRC_BASE + LRFDTRC_O_CH1CMD + sizeof(uint32_t) * channelIndex) = tracerCommand;

    /* Enable interrupts */
    HwiP_restore(key);
}

/*
 *  ======== LogSinkTraceLPF3_printf ========
 */
void LogSinkTraceLPF3_printf(const Log_Module *handle,
                             uint32_t headerPtr,
                             uint32_t numArgs,
                             va_list argptr)
{
    uint32_t arg01 = 0;
    uint32_t arg23 = 0;

    /* Prevent out of bounds access to nArgLut */
    if (numArgs > MAX_ARG_COUNT)
    {
        numArgs = MAX_ARG_COUNT;
    }

    /* Pack the arguments the way the tracer hardware consumes them: full
     * 32-bit words for up to two arguments, 16-bit halves for three or four
     * (see nArgLut). */
    switch (numArgs)
    {
        case 0:
            break;

        case 1:
            arg01 = va_arg(argptr, uintptr_t);
            break;

        case 2:
            arg01 = va_arg(argptr, uintptr_t);
            arg23 = va_arg(argptr, uintptr_t);
            break;

        case 3:
            arg01 = va_arg(argptr, uintptr_t) & 0xFFFFU;
            arg01 |= va_arg(argptr, uintptr_t) << 16;
            arg23 = va_arg(argptr, uintptr_t) & 0xFFFFU;
            break;

        case 4:
        default:
            arg01 = va_arg(argptr, uintptr_t) & 0xFFFFU;
            arg01 |= va_arg(argptr, uintptr_t) << 16;
            arg23 = va_arg(argptr, uintptr_t) & 0xFFFFU;
            arg23 |= va_arg(argptr, uintptr_t) << 16;
            break;
    }

    LogSinkTraceLPF3_send(headerPtr, numArgs, arg01, arg23);
}

/* The fixed-argument-count delegates below are non-variadic (matching the
 * per-arity Log_printfN_fxn typedefs) so their prologue does not spill the
 * argument registers the way a variadic function must. Each runs the level
 * filter and, when it passes, packs its arguments the way the tracer hardware
 * consumes them and calls the shared emit path.
 */

/*
 *  ======== LogSinkTraceLPF3_printfSingleton0 ========
 */
void LogSinkTraceLPF3_printfSingleton0(const Log_Module *handle, Log_Level level, uint32_t headerPtr)
{
    if (LogSinkTraceLPF3_LEVEL_ENABLED(handle, level))
    {
        LogSinkTraceLPF3_send(headerPtr, 0, 0, 0);
    }
}

/*
 *  ======== LogSinkTraceLPF3_printfSingleton1 ========
 */
void LogSinkTraceLPF3_printfSingleton1(const Log_Module *handle, Log_Level level, uint32_t headerPtr, uintptr_t a0)
{
    if (LogSinkTraceLPF3_LEVEL_ENABLED(handle, level))
    {
        LogSinkTraceLPF3_send(headerPtr, 1, a0, 0);
    }
}

/*
 *  ======== LogSinkTraceLPF3_printfSingleton2 ========
 */
void LogSinkTraceLPF3_printfSingleton2(const Log_Module *handle,
                                       Log_Level level,
                                       uint32_t headerPtr,
                                       uintptr_t a0,
                                       uintptr_t a1)
{
    if (LogSinkTraceLPF3_LEVEL_ENABLED(handle, level))
    {
        LogSinkTraceLPF3_send(headerPtr, 2, a0, a1);
    }
}

/*
 *  ======== LogSinkTraceLPF3_printfSingleton3 ========
 */
void LogSinkTraceLPF3_printfSingleton3(const Log_Module *handle,
                                       Log_Level level,
                                       uint32_t headerPtr,
                                       uintptr_t a0,
                                       uintptr_t a1,
                                       uintptr_t a2)
{
    if (LogSinkTraceLPF3_LEVEL_ENABLED(handle, level))
    {
        /* Three arguments travel as 16-bit halves (see nArgLut) */
        LogSinkTraceLPF3_send(headerPtr, 3, (a0 & 0xFFFFU) | (a1 << 16), a2 & 0xFFFFU);
    }
}

/*
 *  ======== LogSinkTraceLPF3_printfSingleton ========
 */
void LogSinkTraceLPF3_printfSingleton(const Log_Module *handle,
                                      Log_Level level,
                                      uint32_t headerPtr,
                                      uint32_t numArgs,
                                      ...)
{
    if (LogSinkTraceLPF3_LEVEL_ENABLED(handle, level))
    {
        va_list argptr;

        va_start(argptr, numArgs);
        LogSinkTraceLPF3_printf(handle, headerPtr, numArgs, argptr);
        va_end(argptr);
    }
}

/*
 *  ======== LogSinkTraceLPF3_bufSingleton ========
 */
void LogSinkTraceLPF3_bufSingleton(const Log_Module *handle,
                                   Log_Level level,
                                   uint32_t headerPtr,
                                   uint8_t *data,
                                   size_t size)
{
    /* This function is not implemented due to lack of HW support */
}

/*
 *  ======== LogSinkTraceLPF3_postNotify ========
 */
static int_fast16_t LogSinkTraceLPF3_postNotify(uint_fast16_t eventType, uintptr_t eventArg, uintptr_t clientArg)
{
    /* Reconfigure the hardware if returning from sleep */
    if (eventType == (uint32_t)PowerLPF3_AWAKE_STANDBY)
    {
        LogSinkTraceLPF3_enable();
    }

    return Power_NOTIFYDONE;
}