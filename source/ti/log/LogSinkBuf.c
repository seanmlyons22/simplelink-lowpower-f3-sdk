/*
 * Copyright (c) 2019-2024, Texas Instruments Incorporated - http://www.ti.com
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
 *  ======== LogSinkBuf.c ========
 *
 *  Records are packed into a circular byte buffer as COBS frames and streamed out
 *  by a host over the debug port. The heavy lifting - mapping the 16-bit id back
 *  to a format string, decoding arguments, and turning timestamp deltas into
 *  absolute time - all happens on the host from the .out file, so the target only
 *  ever writes a couple of bytes of id, a delta, and the raw arguments. See the
 *  record layout in LogSinkBuf.h.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include <ti/drivers/dpl/HwiP.h>
#include <ti/drivers/dpl/TimestampP.h>

#include <ti/log/LogSinkBuf.h>

/* Global LogSinkBuf instance reference for use with singleton implementations
 * of printf.
 */
Log_SINK_BUF_USE(CONFIG_ti_log_LogSinkBuf_0);

/*
 *  ======== LogSinkBuf_uleb ========
 *  ULEB128-encode a 32-bit value into out (at most 5 bytes). Returns the number
 *  of bytes written.
 */
static uint32_t LogSinkBuf_uleb(uint32_t value, uint8_t *out)
{
    uint32_t n = 0;
    while (value >= 0x80)
    {
        out[n++] = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    out[n++] = (uint8_t)value;
    return n;
}

/*
 *  ======== LogSinkBuf_ulebLen ========
 *  Number of bytes LogSinkBuf_uleb() would write for value. Used to size the
 *  reservation before encoding.
 */
static uint32_t LogSinkBuf_ulebLen(uint32_t value)
{
    uint32_t n = 1;
    while (value >= 0x80)
    {
        value >>= 7;
        n++;
    }
    return n;
}

/*
 *  ======== COBS streaming encoder ========
 *
 *  Consistent Overhead Byte Stuffing writes a frame that contains no 0x00 byte,
 *  then a single 0x00 delimiter, so the host can find frame boundaries in the raw
 *  buffer without any length field. This encoder writes straight into the
 *  circular buffer (wrapping at the physical end) so a large Log_buf payload never
 *  needs a matching stack buffer. The code byte at the head of each block is
 *  back-patched once the block length is known.
 */
typedef struct
{
    uint8_t *buf;
    uint32_t size;
    uint32_t wr;      /* ring index of the next output byte */
    uint32_t codeIdx; /* ring index of the pending (not yet written) code byte */
    uint8_t  code;    /* number of bytes in the current block, plus one */
    uint32_t count;   /* total bytes written to the ring so far */
} LogSinkBuf_Cobs;

static void LogSinkBuf_cobsPut(LogSinkBuf_Cobs *c, uint8_t b)
{
    c->buf[c->wr] = b;
    if (++c->wr == c->size)
    {
        c->wr = 0;
    }
    c->count++;
}

static void LogSinkBuf_cobsInit(LogSinkBuf_Cobs *c, LogSinkBuf_Handle inst, uint32_t off)
{
    c->buf     = inst->buffer;
    c->size    = inst->size;
    c->codeIdx = off % inst->size; /* first code byte, back-patched by the first block */
    c->wr      = c->codeIdx;
    if (++c->wr == c->size) /* reserve the code slot */
    {
        c->wr = 0;
    }
    c->code  = 1;
    c->count = 1; /* the reserved code slot counts toward the frame length */
}

static void LogSinkBuf_cobsFeed(LogSinkBuf_Cobs *c, const uint8_t *in, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
    {
        if (in[i] != 0)
        {
            LogSinkBuf_cobsPut(c, in[i]);
            if (++c->code != 0xFF)
            {
                continue;
            }
        }
        /* A zero byte, or a full 254-byte run: close the block and open a new
         * one. The zero itself is represented by the code byte, not emitted. */
        c->buf[c->codeIdx] = c->code;
        c->codeIdx         = c->wr;
        if (++c->wr == c->size)
        {
            c->wr = 0;
        }
        c->count++;
        c->code = 1;
    }
}

static void LogSinkBuf_cobsFinish(LogSinkBuf_Cobs *c)
{
    c->buf[c->codeIdx] = c->code; /* back-patch the final block (slot already counted) */
    LogSinkBuf_cobsPut(c, 0x00);  /* frame delimiter */
}

/*
 *  ======== LogSinkBuf_printf ========
 */
void LogSinkBuf_printf(LogSinkBuf_Handle inst, uint32_t header, uint32_t index, uint32_t numArgs, va_list argptr)
{
    uintptr_t       key;
    uint32_t        now, delta, off, frameLen, i, argLen;
    uint32_t        deltaLen;
    uint8_t         idbuf[2];
    uint8_t         deltabuf[5];
    uint8_t         argbuf[5]; /* one arg's worst-case ULEB, reused per arg */
    uint32_t        argsLen = 0;
    va_list         argCount;
    LogSinkBuf_Cobs cobs;

    (void)header;

    if (numArgs > LogSinkBuf_MAX_ARGS)
    {
        numArgs = LogSinkBuf_MAX_ARGS;
    }

    /* Read the timestamp outside the critical section; only the delta bookkeeping
     * and the reservation below must be atomic. */
    now = TimestampP_getNative32();

    /* Measure the encoded argument length without storing it, so the reservation
     * can be exact. va_copy lets the encode pass below re-walk the same args, so
     * they go straight into the ring through a 5-byte per-arg scratch instead of a
     * LogSinkBuf_MAX_ARGS * 5 stack buffer. */
    va_copy(argCount, argptr);
    for (i = 0; i < numArgs; i++)
    {
        argsLen += LogSinkBuf_ulebLen((uint32_t)va_arg(argCount, uintptr_t));
    }
    va_end(argCount);

    key = HwiP_disable();

    /* Delta from the previous record. The clamp keeps records ordered if a
     * higher-priority context preempted us between the read above and here, and
     * folds the ~9.5 hour RTC wrap into a single zero delta rather than a huge
     * one. */
    if (now >= inst->lastTs)
    {
        delta        = now - inst->lastTs;
        inst->lastTs = now;
    }
    else
    {
        delta = 0;
    }
    deltaLen = LogSinkBuf_ulebLen(delta);

    /* id + delta + args, plus one COBS overhead byte and the 0x00 delimiter. The
     * COBS overhead is exactly one byte because the payload is well under 254. */
    frameLen = 2 + deltaLen + argsLen + 2;
    off      = inst->wrReserve;

    if (frameLen > inst->size)
    {
        HwiP_restore(key);
        return; /* record can never fit */
    }
    if (inst->bufType == LogSinkBuf_Type_LINEAR && (off + frameLen) > inst->size)
    {
        inst->overflow++;
        HwiP_restore(key);
        return; /* linear buffer is full */
    }

    inst->wrReserve = off + frameLen;
    inst->recCount++;

    HwiP_restore(key);

    /* Encode [id][delta][args] as one COBS frame into the reserved bytes. */
    idbuf[0] = (uint8_t)index;
    idbuf[1] = (uint8_t)(index >> 8);
    (void)LogSinkBuf_uleb(delta, deltabuf);

    LogSinkBuf_cobsInit(&cobs, inst, off);
    LogSinkBuf_cobsFeed(&cobs, idbuf, 2);
    LogSinkBuf_cobsFeed(&cobs, deltabuf, deltaLen);
    for (i = 0; i < numArgs; i++)
    {
        argLen = LogSinkBuf_uleb((uint32_t)va_arg(argptr, uintptr_t), argbuf);
        LogSinkBuf_cobsFeed(&cobs, argbuf, argLen);
    }
    LogSinkBuf_cobsFinish(&cobs);
}

/*
 *  ======== LogSinkBuf_printfDepInjection ========
 *
 *  This printf implementation is for use when multiple LogSinkBuf_Instance
 *  instances should be supported within an application.
 *
 *  The implementation will read out the LogSinkBuf_Instance address at runtime
 *  from the handle argument.
 */
void LogSinkBuf_printfDepInjection(const Log_Module *handle, uint32_t header, uint32_t index, uint32_t numArgs, ...)
{
    va_list argptr;

    /* Guard against more arguments being passed in than supported */
    if (numArgs > LogSinkBuf_MAX_ARGS)
    {
        numArgs = LogSinkBuf_MAX_ARGS;
    }

    LogSinkBuf_Handle inst = (LogSinkBuf_Handle)handle->sinkConfig;

    /* Get the VA args pointer in the initial wrapper since you cannot pass VA
     * args to further functions using elipses (...) syntax.
     *
     * All va_start() does is get us the pointer to the first VA arg on the
     * stack. That value will still be valid when passed on further.
     */
    va_start(argptr, numArgs);

    LogSinkBuf_printf(inst, header, index, numArgs, argptr);

    va_end(argptr);
}

/*
 *  ======== LogSinkBuf_printfDepInjection0 ========
 */
void LogSinkBuf_printfDepInjection0(const Log_Module *handle, uint32_t header, uint32_t index, ...)
{
    va_list argptr;

    va_start(argptr, index);
    LogSinkBuf_printf((LogSinkBuf_Handle)handle->sinkConfig, header, index, 0, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkBuf_printfDepInjection1 ========
 */
void LogSinkBuf_printfDepInjection1(const Log_Module *handle, uint32_t header, uint32_t index, ...)
{
    va_list argptr;

    va_start(argptr, index);
    LogSinkBuf_printf((LogSinkBuf_Handle)handle->sinkConfig, header, index, 1, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkBuf_printfDepInjection2 ========
 */
void LogSinkBuf_printfDepInjection2(const Log_Module *handle, uint32_t header, uint32_t index, ...)
{
    va_list argptr;

    va_start(argptr, index);
    LogSinkBuf_printf((LogSinkBuf_Handle)handle->sinkConfig, header, index, 2, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkBuf_printfDepInjection3 ========
 */
void LogSinkBuf_printfDepInjection3(const Log_Module *handle, uint32_t header, uint32_t index, ...)
{
    va_list argptr;

    va_start(argptr, index);
    LogSinkBuf_printf((LogSinkBuf_Handle)handle->sinkConfig, header, index, 3, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkBuf_printfSingleton ========
 *
 *  This implementation purposefully does not use the handle argument but
 *  instead references a single, global LogSinkBuf_Instance structure. When LTO
 *  is enabled, this allows the compiler to avoid loading the handle at the
 *  call site, saving on flash.
 *
 *  Use of this printf implementation has the limitation that only one
 *  LogSinkBuf instance may be used and it will be assigned the
 *  LogsinkBuf_Instance name LogSinkBuf_CONFIG_ti_log_LogSinkBuf_0_config
 */
void LogSinkBuf_printfSingleton(const Log_Module *handle, uint32_t header, uint32_t index, uint32_t numArgs, ...)
{
    va_list argptr;

    /* Guard against more arguments being passed in than supported */
    if (numArgs > LogSinkBuf_MAX_ARGS)
    {
        numArgs = LogSinkBuf_MAX_ARGS;
    }

    /* Get the VA args pointer in the initial wrapper since you cannot pass VA
     * args to further functions using elipses (...) syntax.
     *
     * All va_start() does is get us the pointer to the first VA arg on the
     * stack. That value will still be valid when passed on further.
     */
    va_start(argptr, numArgs);
    LogSinkBuf_printf(&LogSinkBuf_CONFIG_ti_log_LogSinkBuf_0_config, header, index, numArgs, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkBuf_printfSingleton0 ========
 */
void LogSinkBuf_printfSingleton0(const Log_Module *handle, uint32_t header, uint32_t index, ...)
{
    va_list argptr;

    va_start(argptr, index);
    LogSinkBuf_printf(&LogSinkBuf_CONFIG_ti_log_LogSinkBuf_0_config, header, index, 0, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkBuf_printfSingleton1 ========
 */
void LogSinkBuf_printfSingleton1(const Log_Module *handle, uint32_t header, uint32_t index, ...)
{
    va_list argptr;

    va_start(argptr, index);
    LogSinkBuf_printf(&LogSinkBuf_CONFIG_ti_log_LogSinkBuf_0_config, header, index, 1, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkBuf_printfSingleton2 ========
 */
void LogSinkBuf_printfSingleton2(const Log_Module *handle, uint32_t header, uint32_t index, ...)
{
    va_list argptr;

    va_start(argptr, index);
    LogSinkBuf_printf(&LogSinkBuf_CONFIG_ti_log_LogSinkBuf_0_config, header, index, 2, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkBuf_printfSingleton3 ========
 */
void LogSinkBuf_printfSingleton3(const Log_Module *handle, uint32_t header, uint32_t index, ...)
{
    va_list argptr;

    va_start(argptr, index);
    LogSinkBuf_printf(&LogSinkBuf_CONFIG_ti_log_LogSinkBuf_0_config, header, index, 3, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkBuf_bufDepInjection ========
 */
void LogSinkBuf_bufDepInjection(const Log_Module *handle, uint32_t header, uint32_t index, uint8_t *data, size_t size)
{
    uintptr_t       key;
    uint32_t        now, delta, off, payloadLen, worst;
    uint32_t        deltaLen, lenLen;
    uint8_t         hdr[2 + 5 + 5]; /* id + delta + length */
    uint32_t        hdrLen = 0;
    LogSinkBuf_Cobs cobs;

    (void)header;

    if (handle == NULL)
    {
        return;
    }

    LogSinkBuf_Handle inst = (LogSinkBuf_Handle)handle->sinkConfig;

    now = TimestampP_getNative32();

    key = HwiP_disable();

    if (now >= inst->lastTs)
    {
        delta        = now - inst->lastTs;
        inst->lastTs = now;
    }
    else
    {
        delta = 0;
    }
    deltaLen = LogSinkBuf_ulebLen(delta);
    lenLen   = LogSinkBuf_ulebLen((uint32_t)size);

    /* The payload can exceed 254 bytes, so reserve the COBS worst case (one
     * overhead byte per 254-byte block) and pad any slack with delimiters. */
    payloadLen = 2 + deltaLen + lenLen + (uint32_t)size;
    worst      = payloadLen + payloadLen / 254 + 2;
    off        = inst->wrReserve;

    if (worst > inst->size)
    {
        HwiP_restore(key);
        return; /* record can never fit */
    }
    if (inst->bufType == LogSinkBuf_Type_LINEAR && (off + worst) > inst->size)
    {
        inst->overflow++;
        HwiP_restore(key);
        return; /* linear buffer is full */
    }

    inst->wrReserve = off + worst;
    inst->recCount++;

    HwiP_restore(key);

    /* [id][delta][len] followed by the raw payload, all one COBS frame. */
    hdr[hdrLen++] = (uint8_t)index;
    hdr[hdrLen++] = (uint8_t)(index >> 8);
    hdrLen += LogSinkBuf_uleb(delta, &hdr[hdrLen]);
    hdrLen += LogSinkBuf_uleb((uint32_t)size, &hdr[hdrLen]);

    LogSinkBuf_cobsInit(&cobs, inst, off);
    LogSinkBuf_cobsFeed(&cobs, hdr, hdrLen);
    LogSinkBuf_cobsFeed(&cobs, data, (uint32_t)size);
    LogSinkBuf_cobsFinish(&cobs);

    /* Pad the reserved-but-unused tail with delimiters (empty frames the host
     * skips) so the write frontier lines up with the reservation. */
    while (cobs.count < worst)
    {
        LogSinkBuf_cobsPut(&cobs, 0x00);
    }
}
