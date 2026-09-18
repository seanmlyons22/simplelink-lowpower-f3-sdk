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

#ifndef ti_drivers_rcl_RCL_Dma__include
#define ti_drivers_rcl_RCL_Dma__include

#include <stdint.h>

#include <ti/devices/DeviceFamily.h>
#include <ti/drivers/rcl/RCL_Buffer.h>

#if (DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX)

/**
 *  @brief Result of posting a buffer to the DMA data path
 */
typedef enum RCL_Dma_Status_e {
    RCL_Dma_Status_Success = 0,     /*!< Buffer was accepted */
    RCL_Dma_Status_Error_Param,     /*!< Buffer was NULL or too large for one DMA transfer */
    RCL_Dma_Status_Error_Busy,      /*!< A buffer is already in use in this direction */
} RCL_Dma_Status;

/** @defgroup dmaApiFunctions DMA Data Path APIs
 *  These functions are useful as part of the API to RCL
 *  @{
 */

/**
 *  @brief  Post a TX buffer to be moved into the TX FIFO by DMA
 *
 *  The buffer is transferred by the uDMA, paced by the TX FIFO, once a generic
 *  TX command has prepared the FIFO. The buffer may be posted before the command
 *  is submitted or while it is running. The buffer is marked
 *  %RCL_BufferStateFinished when the command ends.
 *
 *  @param  txBuffer  TX buffer to transmit; must remain valid until the command ends
 *
 *  @return %RCL_Dma_Status_Success, or an error status
 */
int_fast16_t RCL_Dma_putTxBuffer(RCL_Buffer_TxBuffer *txBuffer);

/**
 *  @brief  Post an RX buffer for received packets to be moved into by DMA
 *
 *  Received data entries are written back to back into the buffer by the uDMA,
 *  paced by the RX FIFO. The buffer stays posted across commands until replaced.
 *
 *  @param  rxBuffer  Multi buffer to receive into; must remain valid while posted
 *
 *  @return %RCL_Dma_Status_Success, or an error status
 */
int_fast16_t RCL_Dma_putRxBuffer(RCL_MultiBuffer *rxBuffer);

/*!
 *  @brief  Set how much the TX transfer moves per arbitration
 *
 *  This also sets how much data the TX FIFO carries, because the FIFO threshold
 *  is derived from it: the radio asks for more once fewer than this many bytes
 *  are left, so the FIFO runs at roughly one chunk and the modulator paces the
 *  refill. A larger chunk buffers more against DMA latency, a smaller one keeps
 *  less of the packet in the radio.
 *
 *  Takes effect at the next command. The default is 32 bytes.
 *
 *  @param  numBytes  Chunk size; a power of two from 2 to 1024
 *
 *  @return %RCL_Dma_Status_Success, or %RCL_Dma_Status_Error_Param
 */
int_fast16_t RCL_Dma_setTxChunkSize(uint32_t numBytes);
/** @}
 */

/** @defgroup dmaHandlerFunctions DMA Data Path Handler Functions
 *  These functions are meant mostly to be used by handlers and RCL itself
 *  @{
 */

/**
 *  @brief  Set up the uDMA channel and trigger route used by the LRF FIFOs
 *
 *  @note This function is intended as internal to RCL and its handlers
 */
void RCL_Dma_open(void);

/**
 *  @brief  Release the uDMA channel used by the LRF FIFOs
 *
 *  @note This function is intended as internal to RCL and its handlers
 */
void RCL_Dma_close(void);

/**
 *  @brief  Arm the DMA path for TX; to be called after %LRF_prepareTxFifo
 *
 *  Starts the transfer at once if a TX buffer is posted; otherwise the transfer
 *  starts when one is posted.
 *
 *  @note This function is intended as internal to RCL and its handlers
 */
void RCL_Dma_armTx(void);

/**
 *  @brief  Arm the DMA path for RX; to be called after %LRF_prepareRxFifo
 *
 *  Arms a transfer for the writable span of the posted RX buffer and limits the
 *  effective RX FIFO size to match. With no buffer posted, the FIFO is given no
 *  space until one is posted.
 *
 *  @note This function is intended as internal to RCL and its handlers
 */
void RCL_Dma_armRx(void);

/**
 *  @brief  Commit received bytes and re-arm RX; to be called at rxOk/rxNok
 *
 *  @note This function is intended as internal to RCL and its handlers
 *
 *  @return Number of bytes committed to the RX buffer; 0 if none landed
 */
uint32_t RCL_Dma_finishRx(void);

/**
 *  @brief  Stop the DMA path; to be called when the command ends
 *
 *  Disables the channel and finishes the posted TX buffer. The RX buffer stays
 *  posted.
 *
 *  @note This function is intended as internal to RCL and its handlers
 */
void RCL_Dma_stop(void);
/** @}
 */

#endif /* DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX */

#endif /* ti_drivers_rcl_RCL_Dma__include */
