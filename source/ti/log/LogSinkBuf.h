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
 *  ======== LogSinkBuf.h ========
 */

#ifndef ti_loggers_utils_LogSinkBuf__include
#define ti_loggers_utils_LogSinkBuf__include

#include <ti/log/Log.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define Log_TI_LOG_SINK_BUF_VERSION 0.3.0

#define LogSinkBuf_Type_LINEAR   (1)
#define LogSinkBuf_Type_CIRCULAR (2)

/* Most printf arguments one record can carry (matches _Log_NUMARGS in Log.h). */
#define LogSinkBuf_MAX_ARGS (8)

/*
 *  ======== LogSinkBuf_Instance ========
 *
 *  Records are written as a byte stream into a circular buffer and read out by a
 *  host over the debug port while the target keeps running. Each record is a COBS
 *  frame terminated by a 0x00 byte; after COBS-decoding the payload is:
 *
 *      [ id : 2 bytes little-endian ]   low 16 bits of the .log_ptr slot
 *      [ ts : ULEB128 ]                 timestamp delta from the previous record
 *      [ args... : ULEB128 each ]       promoted 32-bit printf arguments
 *
 *  A Log_buf record replaces the arguments with [ len : ULEB128 ][ len raw bytes ].
 *  The host recovers the format string, argument count and everything else about
 *  the log site from the .out file using the id, so nothing describing the site is
 *  kept in RAM.
 *
 *  Only wrReserve advances on the target. The host reads it to find the write
 *  frontier, reads lastTs to anchor the timestamp deltas to absolute time, and
 *  reads recCount (records ever committed) to count any records that were
 *  overwritten before it read them. The trailing 0x00 delimiter doubles as the
 *  commit marker: a half-written or overwritten frame simply fails to COBS-decode
 *  and the host resynchronizes at the next 0x00.
 */
typedef struct LogSinkBuf_Instance
{
    uint8_t          *buffer;    /* circular byte buffer */
    uint32_t          size;      /* size of buffer in bytes */
    volatile uint32_t wrReserve; /* running write offset; ring index is wrReserve % size */
    volatile uint32_t lastTs;    /* timestamp of the most recent record (delta anchor) */
    volatile uint32_t recCount;  /* records ever committed; lets the host count drops */
    volatile uint32_t overflow;  /* records dropped because a LINEAR buffer was full */
    uint8_t           bufType;   /* LogSinkBuf_Type_LINEAR or LogSinkBuf_Type_CIRCULAR */
} LogSinkBuf_Instance;

/*
 *  ======== LogSinkBuf_Handle ========
 */
typedef LogSinkBuf_Instance *LogSinkBuf_Handle;

/*!
 *  @cond NODOC
 *  @brief  Marshal and store a #Log_printf statement into a ring buffer.
 *
 *  Function to marshal a #Log_printf statement into a packet
 *  and store it into a ring buffer. If the packet would overflow
 *  the ring buffer, it will overwrite a previous entry.
 *
 *  This is a singleton implementation. It assumes that there is only one
 *  #LogSinkBuf_Instance object in the application and that this instance is called
 *  LogSinkBuf_CONFIG_ti_log_LogSinkBuf_0_config.
 *
 *  This allows the toolchain in an LTO-enabled application to avoid generating
 *  instructions that load the @c handle since it is not needed.
 *
 *  This implementation should not be used when multiple #LogSinkBuf_Instance
 *  instances are present within the system.
 *
 *  @note Applications must not call this function directly. This is a helper
 *  function to implement #Log_printf
 *
 *  @param[in]  handle     Unused handle
 *
 *  @param[in]  header     Metadata pointer
 *
 *  @param[in]  headerPtr  Unused pointer to metadata pointer
 *
 *  @param[in]  numArgs    Number of arguments
 *
 *  @param[in]  ...        Variable number of arguments
 */
extern void LogSinkBuf_printfSingleton(const Log_Module *handle,
                                       uint32_t header,
                                       uint32_t index,
                                       uint32_t numArgs,
                                       ...);

extern void LogSinkBuf_printfSingleton0(const Log_Module *handle, uint32_t header, uint32_t index, ...);

extern void LogSinkBuf_printfSingleton1(const Log_Module *handle, uint32_t header, uint32_t index, ...);

extern void LogSinkBuf_printfSingleton2(const Log_Module *handle, uint32_t header, uint32_t index, ...);

extern void LogSinkBuf_printfSingleton3(const Log_Module *handle, uint32_t header, uint32_t index, ...);
/*! @endcond NODOC */

/*!
 *  @cond NODOC
 *  @brief  Marshal and store a #Log_printf statement into a ring buffer.
 *
 *  Function to marshal a #Log_printf statement into a packet
 *  and store it into a ring buffer. If the packet would overflow
 *  the ring buffer, it will overwrite a previous entry.
 *
 *  This is a dependency injection implementation. It is able to support an
 *  arbitrary number of LogSinkBuf instances by passing in the sink state
 *  through @c handle. This requires additional flash to load @c handle in each
 *  #Log_printf though.
 *
 *  @note Applications must not call this function directly. This is a helper
 *  function to implement #Log_printf
 *
 *  @param[in]  handle     Handle to the module and sink instance
 *
 *  @param[in]  header     Metadata pointer
 *
 *  @param[in]  headerPtr  Unused pointer to metadata pointer
 *
 *  @param[in]  numArgs    Number of arguments
 *
 *  @param[in]  ...        Variable number of arguments
 */
extern void LogSinkBuf_printfDepInjection(const Log_Module *handle,
                                          uint32_t header,
                                          uint32_t index,
                                          uint32_t numArgs,
                                          ...);

extern void LogSinkBuf_printfDepInjection0(const Log_Module *handle, uint32_t header, uint32_t index, ...);

extern void LogSinkBuf_printfDepInjection1(const Log_Module *handle, uint32_t header, uint32_t index, ...);

extern void LogSinkBuf_printfDepInjection2(const Log_Module *handle, uint32_t header, uint32_t index, ...);

extern void LogSinkBuf_printfDepInjection3(const Log_Module *handle, uint32_t header, uint32_t index, ...);
/*! @endcond NODOC */

/*!
 *  @cond NODOC
 *  @brief  Marshal and store a #Log_buf statement into a ring buffer.
 *
 *  Function to marshal a #Log_buf statement into a packet
 *  and store it into a ring buffer. If the packet would overflow
 *  the ring buffer, it will overwrite a previous entry.
 *
 *  This is a dependency injection implementation. It is able to support an
 *  arbitrary number of LogSinkBuf instances by passing in the sink state
 *  through @c handle.
 *
 *  @note Applications must not call this function directly. This is a helper
 *  function to implement #Log_buf
 *
 *  @param[in]  handle     LogSinkBuf sink handle
 *
 *  @param[in]  header     Metadata pointer
 *
 *  @param[in]  headerPtr  Unused pointer to metadata pointer
 *
 *  @param[in]  data       Data buffer to log
 *
 *  @param[in]  size       Size in bytes of array to store
 */
extern void LogSinkBuf_bufDepInjection(const Log_Module *handle,
                                       uint32_t header,
                                       uint32_t index,
                                       uint8_t *data,
                                       size_t size);
/*! @endcond NODOC */

/*
 * Helpers to define/use instance.
 */
#define Log_SINK_BUF_DEFINE(name, type, num_bytes)                          \
    static uint8_t logSinkBuf_##name##_buffer[num_bytes];                   \
    LogSinkBuf_Instance LogSinkBuf_##name##_config = {                      \
        .buffer    = logSinkBuf_##name##_buffer,                            \
        .size      = num_bytes,                                             \
        .wrReserve = 0,                                                     \
        .lastTs    = 0,                                                     \
        .recCount  = 0,                                                     \
        .overflow  = 0,                                                     \
        .bufType   = type}
#define Log_SINK_BUF_USE(name) extern LogSinkBuf_Instance LogSinkBuf_##name##_config
#define Log_MODULE_INIT_SINK_BUF(name, _levels, printfDelegate, bufDelegate, _dynamicLevelsPtr)                       \
    {                                                                                                                 \
        .sinkConfig = &LogSinkBuf_##name##_config, .printf = printfDelegate, .printf0 = printfDelegate##0,            \
        .printf1 = printfDelegate##1, .printf2 = printfDelegate##2, .printf3 = printfDelegate##3, .buf = bufDelegate, \
        .levels = _levels, .dynamicLevelsPtr = _dynamicLevelsPtr,                                                     \
    }

_Log_DEFINE_LOG_VERSION(LogSinkBuf, Log_TI_LOG_SINK_BUF_VERSION);

#if defined(__cplusplus)
}
#endif

#endif /* ti_loggers_utils_LogSinkBuf__include */
