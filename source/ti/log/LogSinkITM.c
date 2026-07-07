/*
 * Copyright (c) 2022-2025, Texas Instruments Incorporated - http://www.ti.com
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
 *  ======== LogSinkITM.c ========
 */

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>

#include <ti/log/Log.h>
#include <ti/log/LogSinkITM.h>

#include <ti/drivers/dpl/TimestampP.h>
#include <ti/drivers/dpl/HwiP.h>
#include <ti/drivers/ITM.h>

#define LogSinkITM_RESET_FRAME (0xBBBBBBBB)

/* Most printf arguments one record can carry (matches _Log_NUMARGS in Log.h). */
#define LogSinkITM_MAX_ARGS (8)

/* Runtime level filter: emit only when the level is enabled, either through a
 * module's dynamic (runtime-settable) bitmap or its constant one. */
#define LogSinkITM_LEVEL_ENABLED(handle, level) \
    (((handle->dynamicLevelsPtr != NULL) && (level & *(handle->dynamicLevelsPtr))) || (handle->levels & level))

/*
 *  ======== LogSinkITM_sendTimeSync ========
 */
void LogSinkITM_sendTimeSync(void)
{
    uint64_t ts = TimestampP_getNative64();
    ITM_send32Polling(LogSinkITM_STIM_TIME_SYNC, ts & 0xffffffff);
    ITM_send32Polling(LogSinkITM_STIM_TIME_SYNC, ts >> 32);
}

/*
 *  ======== LogSinkITM_init ========
 */
void LogSinkITM_init(void)
{
    /* disable interrupts */
    uint32_t key   = HwiP_disable();
    /* Setup and enable ITM driver */
    bool itmOpened = ITM_open();

    if (itmOpened)
    {
        /* Send the reset sequence */
        ITM_send32Atomic(LogSinkITM_STIM_INFO, LogSinkITM_RESET_FRAME);
        /* Enable generation of time stamps based on system CPU */
        ITM_enableTimestamps(ITM_TS_DIV_16, false);

        /* Send information about timer resync */
        ITM_send16Polling(LogSinkITM_STIM_INFO, LogSinkITM_Info_Timing | ITM_TS_DIV_16 << 8);
        ITM_send32Polling(LogSinkITM_STIM_INFO, TimestampP_nativeFormat64.value);

        /* Initial timestamp sync  */
        LogSinkITM_sendTimeSync();
    }

    /* enable interrupts */
    HwiP_restore(key);
}

/*
 *  ======== LogSinkITM_emit ========
 *  Shared emit path. Takes the promoted printf arguments as a plain array so the
 *  fixed-argument-count wrappers can reach it without building a va_list. Level
 *  filtering is done by the wrappers.
 */
static void LogSinkITM_emit(uint32_t headerPtr, const uintptr_t *args, uint32_t numArgs)
{
    uint32_t key;

    /* disable interrupts */
    key = HwiP_disable();

    /*
     * The log site is named by its .log_ptr slot. The host recovers the full
     * slot from the .out file, so only the low 16 bits are needed to identify
     * it. Send a halfword to keep the header transfer short.
     */
    ITM_send16Polling(LogSinkITM_STIM_HEADER, (uint16_t)headerPtr);

    uint32_t i;
    for (i = 0; i < numArgs; ++i)
    {
        ITM_send32Polling(LogSinkITM_STIM_TRACE, args[i]);
    }

    /* enable interrupts */
    HwiP_restore(key);
}

/* The fixed-argument-count delegates below are non-variadic (matching the
 * per-arity Log_printfN_fxn typedefs) so their prologue does not spill the
 * argument registers the way a variadic function must. Each runs the level
 * filter and, when it passes, calls the shared emit path with a small argument
 * array built on the stack.
 */

/*
 *  ======== LogSinkITM_printfSingleton0 ========
 */
void LogSinkITM_printfSingleton0(const Log_Module *handle, Log_Level level, uint32_t headerPtr)
{
    if (LogSinkITM_LEVEL_ENABLED(handle, level))
    {
        LogSinkITM_emit(headerPtr, NULL, 0);
    }
}

/*
 *  ======== LogSinkITM_printfSingleton1 ========
 */
void LogSinkITM_printfSingleton1(const Log_Module *handle, Log_Level level, uint32_t headerPtr, uintptr_t a0)
{
    if (LogSinkITM_LEVEL_ENABLED(handle, level))
    {
        LogSinkITM_emit(headerPtr, &a0, 1);
    }
}

/*
 *  ======== LogSinkITM_printfSingleton2 ========
 */
void LogSinkITM_printfSingleton2(const Log_Module *handle, Log_Level level, uint32_t headerPtr, uintptr_t a0, uintptr_t a1)
{
    if (LogSinkITM_LEVEL_ENABLED(handle, level))
    {
        uintptr_t argv[2] = {a0, a1};
        LogSinkITM_emit(headerPtr, argv, 2);
    }
}

/*
 *  ======== LogSinkITM_printfSingleton3 ========
 */
void LogSinkITM_printfSingleton3(const Log_Module *handle,
                                 Log_Level level,
                                 uint32_t headerPtr,
                                 uintptr_t a0,
                                 uintptr_t a1,
                                 uintptr_t a2)
{
    if (LogSinkITM_LEVEL_ENABLED(handle, level))
    {
        uintptr_t argv[3] = {a0, a1, a2};
        LogSinkITM_emit(headerPtr, argv, 3);
    }
}

/*
 *  ======== LogSinkITM_printfSingleton ========
 */
void LogSinkITM_printfSingleton(const Log_Module *handle, Log_Level level, uint32_t headerPtr, uint32_t numArgs, ...)
{
    if (LogSinkITM_LEVEL_ENABLED(handle, level))
    {
        va_list   argptr;
        uintptr_t argv[LogSinkITM_MAX_ARGS];
        uint32_t  i;

        if (numArgs > LogSinkITM_MAX_ARGS)
        {
            numArgs = LogSinkITM_MAX_ARGS;
        }

        va_start(argptr, numArgs);
        for (i = 0; i < numArgs; i++)
        {
            argv[i] = va_arg(argptr, uintptr_t);
        }
        va_end(argptr);

        LogSinkITM_emit(headerPtr, argv, numArgs);
    }
}

/*
 *  ======== LogSinkITM_buf ========
 */
void LogSinkITM_bufSingleton(const Log_Module *handle, Log_Level level, uint32_t headerPtr, uint8_t *data, size_t size)
{
    if (LogSinkITM_LEVEL_ENABLED(handle, level))
    {
        uint32_t key;

        /* disable interrupts */
        key = HwiP_disable();

        /* Low 16 bits of the .log_ptr slot are enough to name the log site (see printf) */
        ITM_send16Polling(LogSinkITM_STIM_HEADER, (uint16_t)headerPtr);
        /* We always send the size of the expected buffer */
        ITM_send32Polling(LogSinkITM_STIM_TRACE, size);
        /* Send out the actual data */
        ITM_sendBufferAtomic(LogSinkITM_STIM_TRACE, (const char *)data, size);

        /* enable interrupts */
        HwiP_restore(key);
    }
}

/*
 *  ======== LogSinkITM_finalize ========
 */
void LogSinkITM_finalize(void)
{
    /* disable interrupts */
    uint32_t key = HwiP_disable();

    ITM_close();

    /* enable interrupts */
    HwiP_restore(key);
}
