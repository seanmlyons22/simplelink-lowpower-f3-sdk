/*
 * Copyright (c) 2023-2024 Texas Instruments Incorporated - http://www.ti.com
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
 *  ======== LogSinkUART.c ========
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

#include <ti/log/Log.h>
#include <ti/log/LogSinkUART.h>

#include <ti/drivers/dpl/TimestampP.h>
#include <ti/drivers/dpl/HwiP.h>
#include <ti/drivers/UART2.h>
#include <ti/drivers/utils/RingBuf.h>

/* Each numeric field (timestamp, buffer size and each argument) is 4 bytes. */
#define LogSinkUART_BYTES_PER_FIELD (4)

/*
 * Every record starts with a one-byte frame header so the host can
 * resynchronize to the byte stream without relying on a wide address value: a
 * fixed sync pattern in the high nibble and a record code in the low nibble.
 * For a printf the code is the argument count (0..8); a buffer and an overflow
 * record use the two reserved codes below.
 */
#define LogSinkUART_FRAME_SYNC    (0xA0U)
#define LogSinkUART_CODE_OVERFLOW (0x0EU)
#define LogSinkUART_CODE_BUFFER   (0x0FU)

/* Frame header byte plus the 16-bit log id. */
#define LogSinkUART_ID_HEADER_SIZE (3)

/* A printf record is the id header, a 32-bit timestamp, then 0..8 arguments. */
#define LogSinkUART_PRINTF_HEADER_SIZE (LogSinkUART_ID_HEADER_SIZE + LogSinkUART_BYTES_PER_FIELD)
#define LogSinkUART_PRINTF_MAX_ARGS    (8)
#define LogSinkUART_PRINTF_MAX_SIZE \
    (LogSinkUART_PRINTF_HEADER_SIZE + LogSinkUART_PRINTF_MAX_ARGS * LogSinkUART_BYTES_PER_FIELD)

/* A buffer record adds a 32-bit size field before the payload bytes. */
#define LogSinkUART_BUF_HEADER_SIZE (LogSinkUART_PRINTF_HEADER_SIZE + LogSinkUART_BYTES_PER_FIELD)

/* An overflow record is only the id header. */
#define LogSinkUART_OVERFLOW_PACKET_SIZE (LogSinkUART_ID_HEADER_SIZE)

extern const uint_least8_t LogSinkUART_count;

/*
 *  =========== LogSinkUART_storePacket ==========
 *  Helper function to store a log packet into an intermediate ring buffer. To
 *  make the function thread-safe it should be called from a context where HWI
 *  is disabled.
 */
static void LogSinkUART_storePacket(RingBuf_Handle ringObject, unsigned char *packet, size_t packetLength)
{
    int linearSpace;                    /* Number of unsigned chars available in linear memory of ring buffer */
    unsigned char *dstAddr;             /* Pointer reference to the next chunk of linear memory available */
    size_t bytesWritten = 0;            /* Number of bytes written in the ring buffer */
    size_t writeCount   = packetLength; /* Number of bytes left to write in the ring buffer */

    /* Store packet in a Ring Buffer */
    do
    {
        /* Get the number of contiguous bytes we can copy to the ring
         * buffer and the location where we can start the copy into the
         * ring buffer.
         */
        linearSpace = RingBuf_putPointer(ringObject, &dstAddr);
        if (linearSpace > writeCount)
        {
            linearSpace = writeCount;
        }

        memcpy(dstAddr, packet + bytesWritten, linearSpace);

        /* Update the ring buffer state with the number of bytes copied */
        RingBuf_putAdvance(ringObject, linearSpace);

        writeCount -= linearSpace;
        bytesWritten += linearSpace;
    } while ((linearSpace > 0) && (writeCount > 0));
}

/*
 *  ======== LogSinkUART_flush ========
 */
void LogSinkUART_flush(void)
{
    /* Loop through all the UART2 LogSinks and flush each ring buffer */
    for (size_t i = 0; i < LogSinkUART_count; i++)
    {
        uint32_t key;
        LogSinkUART_Config *config = (LogSinkUART_Config *)&LogSinkUART_config[i];
        LogSinkUART_Object *object = config->object;

        size_t bytesWritten;    /* Number of bytes written to the UART */
        int available;          /* Number of available linear bytes in ring buffer */
        unsigned char *srcAddr; /* Address in ring buffer where data can be read from */
        size_t readCount;       /* Number of bytes left to read */

        key = HwiP_disable();

        /* If there is no data to flush go to the next ring buffer */
        readCount = RingBuf_getCount(&object->ringObj);
        if (readCount == 0)
        {
            HwiP_restore(key);
            continue;
        }

        /* Read data from the ring buffer and put as much as possible on the UART */
        available = RingBuf_getPointer(&object->ringObj, &srcAddr);
        UART2_write(object->uartHandle, srcAddr, available, &bytesWritten);
        RingBuf_getConsume(&object->ringObj, bytesWritten);

        HwiP_restore(key);
    }
}

/*
 *  ======== LogSinkUART_init ========
 */
void LogSinkUART_init(uint_least8_t index)
{
    uint32_t key;
    UART2_Params uartParams;

    LogSinkUART_Config *config         = (LogSinkUART_Config *)&LogSinkUART_config[index];
    LogSinkUART_Object *object         = config->object;
    LogSinkUART_HWAttrs const *hwAttrs = config->hwAttrs;

    /* Disable interrupts */
    key = HwiP_disable();

    /* Construct ring buffer for intermediate storage */
    RingBuf_construct(&object->ringObj, hwAttrs->bufPtr, hwAttrs->bufSize);

    /* Setup and open UART2 */
    UART2_Params_init(&uartParams);

    uartParams.writeMode  = UART2_Mode_NONBLOCKING;
    uartParams.baudRate   = hwAttrs->baudRate;
    uartParams.parityType = hwAttrs->parity;

    object->uartHandle = UART2_open(hwAttrs->uartIndex, &uartParams);

    if (object->uartHandle == NULL)
    {
        /* UART2_open() failed */
        while (1) {}
    }

    /* enable interrupts */
    HwiP_restore(key);
}

/*
 *  ======== LogSinkUART_printf ========
 */
void LogSinkUART_printf(LogSinkUART_Config *config, uint32_t headerPtr, uint32_t numArgs, va_list argptr)
{
    uintptr_t key;
    uint8_t packet[LogSinkUART_PRINTF_MAX_SIZE];
    uint16_t logId = (uint16_t)headerPtr;
    uint32_t timestamp;

    LogSinkUART_Object *object = config->object;

    size_t packetSize = LogSinkUART_PRINTF_HEADER_SIZE + numArgs * LogSinkUART_BYTES_PER_FIELD;

    /* Get faithful timestamp and ensure that we check
     * if we have space for this packet.
     */
    key = HwiP_disable();

    timestamp = TimestampP_getNative32();

    /* Check if the ring buffer is full */
    if (RingBuf_isFull(&object->ringObj))
    {
        HwiP_restore(key);
        return;
    }

    /* Frame header and log id are common to the record and the overflow marker */
    packet[0] = LogSinkUART_FRAME_SYNC | (uint8_t)numArgs;
    packet[1] = (uint8_t)logId;
    packet[2] = (uint8_t)(logId >> 8);

    /* Assuming that the ring buffer is not full, we check if we
     * have space for the current packet and the overflow packet.
     *
     * If there is space, we proceed normally, and we ensure that
     * if there were not space for the next message, at least there
     * would be space for the overflow message.
     *
     * If there is not enough space, we put an overflow packet into
     * the ring buffer.
     */
    if (RingBuf_space(&object->ringObj) >= packetSize + LogSinkUART_OVERFLOW_PACKET_SIZE)
    {
        memcpy(&packet[LogSinkUART_ID_HEADER_SIZE], &timestamp, LogSinkUART_BYTES_PER_FIELD);

        for (uint32_t i = 0; i < numArgs; i++)
        {
            uint32_t arg = (uint32_t)va_arg(argptr, uintptr_t);
            memcpy(&packet[LogSinkUART_PRINTF_HEADER_SIZE + i * LogSinkUART_BYTES_PER_FIELD],
                   &arg,
                   LogSinkUART_BYTES_PER_FIELD);
        }
    }
    else
    {
        /* Not enough space for the record, so store an overflow marker instead.
         * The log id is kept so the host still knows which statement would have
         * overflowed.
         */
        packet[0]  = LogSinkUART_FRAME_SYNC | LogSinkUART_CODE_OVERFLOW;
        packetSize = LogSinkUART_OVERFLOW_PACKET_SIZE;
    }

    /* Store packet in intermediate storage */
    LogSinkUART_storePacket(&object->ringObj, packet, packetSize);

    HwiP_restore(key);
}

/*
 *  ======== LogSinkUART_printfSingleton ========
 */
void LogSinkUART_printfSingleton(const Log_Module *handle, uint32_t header, uint32_t headerPtr, uint32_t numArgs, ...)
{
    va_list argptr;

    /* Since we assume LogSinkUART is a singleton in this implementation, we can
     * access the zeroth array element directly.
     */
    LogSinkUART_Config *config = (LogSinkUART_Config *)&LogSinkUART_config[0];

    /* Get the VA args pointer in the initial wrapper since you cannot pass VA
     * args to further functions using elipses (...) syntax.
     *
     * All va_start() does is get us the pointer to the first VA arg on the
     * stack. That value will still be valid when passed on further.
     */
    va_start(argptr, numArgs);

    LogSinkUART_printf(config, headerPtr, numArgs, argptr);

    va_end(argptr);
}

/*
 *  ======== LogSinkUART_printfSingleton0 ========
 */
void LogSinkUART_printfSingleton0(const Log_Module *handle, uint32_t header, uint32_t headerPtr, ...)
{
    va_list argptr;

    va_start(argptr, headerPtr);
    LogSinkUART_printf((LogSinkUART_Config *)&LogSinkUART_config[0], headerPtr, 0, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkUART_printfSingleton1 ========
 */
void LogSinkUART_printfSingleton1(const Log_Module *handle, uint32_t header, uint32_t headerPtr, ...)
{
    va_list argptr;

    va_start(argptr, headerPtr);
    LogSinkUART_printf((LogSinkUART_Config *)&LogSinkUART_config[0], headerPtr, 1, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkUART_printfSingleton2 ========
 */
void LogSinkUART_printfSingleton2(const Log_Module *handle, uint32_t header, uint32_t headerPtr, ...)
{
    va_list argptr;

    va_start(argptr, headerPtr);
    LogSinkUART_printf((LogSinkUART_Config *)&LogSinkUART_config[0], headerPtr, 2, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkUART_printfSingleton3 ========
 */
void LogSinkUART_printfSingleton3(const Log_Module *handle, uint32_t header, uint32_t headerPtr, ...)
{
    va_list argptr;

    va_start(argptr, headerPtr);
    LogSinkUART_printf((LogSinkUART_Config *)&LogSinkUART_config[0], headerPtr, 3, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkUART_printfDepInjection ========
 */
void LogSinkUART_printfDepInjection(const Log_Module *handle,
                                    uint32_t header,
                                    uint32_t headerPtr,
                                    uint32_t numArgs,
                                    ...)
{
    va_list argptr;

    /* Since this is a dependency injection implementation, we need to fetch the
     * config pointer from the Log_Module handle.
     */
    LogSinkUART_Handle inst    = (LogSinkUART_Handle)handle->sinkConfig;
    LogSinkUART_Config *config = (LogSinkUART_Config *)&LogSinkUART_config[inst->index];

    /* Get the VA args pointer in the initial wrapper since you cannot pass VA
     * args to further functions using elipses (...) syntax.
     *
     * All va_start() does is get us the pointer to the first VA arg on the
     * stack. That value will still be valid when passed on further.
     */
    va_start(argptr, numArgs);

    LogSinkUART_printf(config, headerPtr, numArgs, argptr);

    va_end(argptr);
}

/*
 *  ======== LogSinkUART_printfDepInjection0 ========
 */
void LogSinkUART_printfDepInjection0(const Log_Module *handle, uint32_t header, uint32_t headerPtr, ...)
{
    va_list argptr;

    LogSinkUART_Handle inst    = (LogSinkUART_Handle)handle->sinkConfig;
    LogSinkUART_Config *config = (LogSinkUART_Config *)&LogSinkUART_config[inst->index];

    va_start(argptr, headerPtr);
    LogSinkUART_printf(config, headerPtr, 0, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkUART_printfDepInjection1 ========
 */
void LogSinkUART_printfDepInjection1(const Log_Module *handle, uint32_t header, uint32_t headerPtr, ...)
{
    va_list argptr;

    LogSinkUART_Handle inst    = (LogSinkUART_Handle)handle->sinkConfig;
    LogSinkUART_Config *config = (LogSinkUART_Config *)&LogSinkUART_config[inst->index];

    va_start(argptr, headerPtr);
    LogSinkUART_printf(config, headerPtr, 1, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkUART_printfDepInjection2 ========
 */
void LogSinkUART_printfDepInjection2(const Log_Module *handle, uint32_t header, uint32_t headerPtr, ...)
{
    va_list argptr;

    LogSinkUART_Handle inst    = (LogSinkUART_Handle)handle->sinkConfig;
    LogSinkUART_Config *config = (LogSinkUART_Config *)&LogSinkUART_config[inst->index];

    va_start(argptr, headerPtr);
    LogSinkUART_printf(config, headerPtr, 2, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkUART_printfDepInjection3 ========
 */
void LogSinkUART_printfDepInjection3(const Log_Module *handle, uint32_t header, uint32_t headerPtr, ...)
{
    va_list argptr;

    LogSinkUART_Handle inst    = (LogSinkUART_Handle)handle->sinkConfig;
    LogSinkUART_Config *config = (LogSinkUART_Config *)&LogSinkUART_config[inst->index];

    va_start(argptr, headerPtr);
    LogSinkUART_printf(config, headerPtr, 3, argptr);
    va_end(argptr);
}

/*
 *  ======== LogSinkUART_bufDepInjection ========
 */
void LogSinkUART_bufDepInjection(const Log_Module *handle,
                                 uint32_t header,
                                 uint32_t headerPtr,
                                 uint8_t *data,
                                 size_t size)
{
    uintptr_t key;
    uint8_t packet[LogSinkUART_BUF_HEADER_SIZE];
    uint16_t logId     = (uint16_t)headerPtr;
    uint32_t sizeField = (uint32_t)size;
    uint32_t timestamp;

    LogSinkUART_Handle inst    = (LogSinkUART_Handle)handle->sinkConfig;
    LogSinkUART_Config *config = (LogSinkUART_Config *)&LogSinkUART_config[inst->index];
    LogSinkUART_Object *object = config->object;

    /* Get faithful timestamp and ensure that we check
     * if we have space for this packet.
     */
    key = HwiP_disable();

    timestamp = TimestampP_getNative32();

    /* Check if the ring buffer is full */
    if (RingBuf_isFull(&object->ringObj))
    {
        HwiP_restore(key);
        return;
    }

    /* Frame header and log id are common to the record and the overflow marker */
    packet[0] = LogSinkUART_FRAME_SYNC | LogSinkUART_CODE_BUFFER;
    packet[1] = (uint8_t)logId;
    packet[2] = (uint8_t)(logId >> 8);

    /* Assuming that the ring buffer is not full, we check if we
     * have space for the current packet and the overflow packet.
     *
     * If there is space, we proceed normally, and we ensure that
     * if there were not space for the next message, at least there
     * would be space for the overflow message.
     *
     * If there is not enough space, we put an overflow packet into
     * the ring buffer.
     */
    if (RingBuf_space(&object->ringObj) >= LogSinkUART_BUF_HEADER_SIZE + size + LogSinkUART_OVERFLOW_PACKET_SIZE)
    {
        memcpy(&packet[LogSinkUART_ID_HEADER_SIZE], &timestamp, LogSinkUART_BYTES_PER_FIELD);
        memcpy(&packet[LogSinkUART_PRINTF_HEADER_SIZE], &sizeField, LogSinkUART_BYTES_PER_FIELD);
        LogSinkUART_storePacket(&object->ringObj, packet, LogSinkUART_BUF_HEADER_SIZE);
        LogSinkUART_storePacket(&object->ringObj, data, size);
    }
    else
    {
        /* Not enough space for the record, so store an overflow marker instead.
         * The log id is kept so the host still knows which statement would have
         * overflowed.
         */
        packet[0] = LogSinkUART_FRAME_SYNC | LogSinkUART_CODE_OVERFLOW;
        LogSinkUART_storePacket(&object->ringObj, packet, LogSinkUART_OVERFLOW_PACKET_SIZE);
    }

    /* enable interrupts */
    HwiP_restore(key);
}

/*
 *  ======== LogSinkUART_finalize ========
 */
void LogSinkUART_finalize(uint_least8_t index)
{
    LogSinkUART_Config *config = (LogSinkUART_Config *)&LogSinkUART_config[index];
    LogSinkUART_Object *object = config->object;

    /* Close the UART peripheral making sure that there are no ongoing writes*/
    UART2_writeCancel(object->uartHandle);
    UART2_close(object->uartHandle);
}
