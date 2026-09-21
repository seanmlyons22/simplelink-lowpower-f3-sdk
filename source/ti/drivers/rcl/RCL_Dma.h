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
    RCL_Dma_Status_Error_Param,     /*!< Buffer was NULL, too large for one DMA transfer, or not laid out as required */
    RCL_Dma_Status_Error_Busy,      /*!< A buffer is already in use in this direction */
    RCL_Dma_Status_Error_Fifo,      /*!< The DMA did not move the first entry into the FIFO in time */
} RCL_Dma_Status;

/** @defgroup dmaApiFunctions DMA Data Path APIs
 *  These functions are useful as part of the API to RCL
 *  @{
 */

/**
 *  @brief  Post the entries of a TX burst to be moved into the TX FIFO by DMA
 *
 *  The entries are data entries (length field, pad and packet, padded to a
 *  word) laid out back to back in memory as they will lie in the FIFO, all of
 *  the same length, and are moved into the TX FIFO one entry per FIFO request
 *  by a generic TX burst command. One arbitration is one entry, so the FIFO
 *  asks for entry k+1 when the PBE starts on entry k and an entry is never
 *  split across two requests. The entry must therefore be a power of two
 *  bytes; a burst whose entries are not, or are not all the same length, is
 *  rejected.
 *
 *  The burst must be posted before the command is submitted. The PBE pops the
 *  header of the first entry the moment the operation is handed to it, at
 *  command setup and long before the start time, and it does not wait for
 *  the FIFO to have anything: the handler therefore moves the first entry in
 *  at setup, before it posts the operation, and ends the command with
 *  %RCL_CommandStatus_Error_MissingTxBuffer if nothing has been posted by
 *  then. The other entries follow one per FIFO request, each as the packet
 *  before it goes on the air. How early the first entry is committed is set
 *  by when the command is submitted; a command submitted late is rejected by
 *  the scheduler, which is a reported error, whereas a FIFO the PBE finds
 *  empty is not: the PBE's own error path then waits for an RFE reply that
 *  never comes and the command never ends. The application may write the
 *  payload of an entry the FIFO has not taken yet, see
 *  %RCL_Dma_getTxEntriesTaken.
 *
 *  @param  entries     First of %numEntries consecutive data entries; must remain valid until the command ends
 *  @param  numEntries  Number of entries in the burst, at least 1
 *
 *  @return %RCL_Dma_Status_Success, or an error status
 */
int_fast16_t RCL_Dma_putTxBurst(RCL_Buffer_DataEntry *entries, uint32_t numEntries);

/**
 *  @brief  Number of posted TX entries the FIFO has taken so far
 *
 *  Counts whole entries moved into the TX FIFO from the posted burst, as
 *  recorded by the uDMA at the end of each arbitration. An entry that has been
 *  taken has been read from memory in full and changing it has no effect on
 *  what is transmitted. An entry that has not been taken may still be written,
 *  in whole words; a word written while the uDMA is reading that entry goes
 *  out either as the old or the new value, never a mix.
 *
 *  Once the command has ended and handed the burst back, every entry of it
 *  counts as taken until the next burst is posted.
 *
 *  @return Entries taken; 0 before the transfer of a posted burst has started
 */
uint32_t RCL_Dma_getTxEntriesTaken(void);

/**
 *  @brief  Post an RX buffer for received packets to be moved into by DMA
 *
 *  Received data entries are written back to back into the buffer by the uDMA,
 *  paced by the RX FIFO. The buffer stays posted across commands until
 *  replaced, and must have room for a whole burst and for a whole RX FIFO;
 *  see %RCL_Dma_armRxBurst.
 *
 *  Call this between commands. A burst commits what it received into whichever
 *  buffer is posted when it ends, so replacing the buffer under a burst that is
 *  already armed is refused with %RCL_Dma_Status_Error_Busy.
 *
 *  @param  rxBuffer  Multi buffer to receive into; must remain valid while posted
 *
 *  @return %RCL_Dma_Status_Success, %RCL_Dma_Status_Error_Busy if a burst is
 *          armed, or an error status
 */
int_fast16_t RCL_Dma_putRxBuffer(RCL_MultiBuffer *rxBuffer);

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
 *  Starts the transfer at once if a burst is posted and waits, bounded, for
 *  the first entry to be in the FIFO. A burst handler must not post the
 *  operation unless this returns %RCL_Dma_Status_Success, since the PBE pops
 *  the first entry's header as soon as it has the operation.
 *
 *  @note This function is intended as internal to RCL and its handlers
 *
 *  @return %RCL_Dma_Status_Success with the first entry in the FIFO, %RCL_Dma_Status_Error_Param with nothing posted, %RCL_Dma_Status_Error_Fifo if the entry did not land in time
 */
int_fast16_t RCL_Dma_armTx(void);

/**
 *  @brief  Arm the DMA drain of an RX burst; to be called after %LRF_prepareRxFifo
 *
 *  The burst of %numEntries entries of %entryBytes each is moved into the
 *  posted RX buffer one entry at a time while it is on the air, on the DMA
 *  request the RX FIFO raises when the PBE commits an entry, and
 *  %RCL_Dma_finishRxBurst takes whatever is left when the command has ended.
 *
 *  A committed entry is one whole DMA arbitration, and one request is answered
 *  with one arbitration, so %entryBytes has to be a power of two bytes and at
 *  least two of them. The burst is not bounded by the size of the FIFO, only
 *  by the posted buffer and by what one uDMA transfer can carry: the FIFO port
 *  advances the read pointer as it is read and the space is handed back at
 *  once.
 *
 *  The commit is what makes this safe. The PBE commits at the end of a packet
 *  and never while it is pushing one; the request is a pulse per entry, so the
 *  transfer is quantised in entries and cannot end up part way through one;
 *  and the arbitration never exceeds what the commit made readable, so a read
 *  never finds the FIFO short. Anything the transfer does not reach stays in
 *  the FIFO, in order and in whole entries, for the drain at the end.
 *
 *  The posted buffer must have room for the burst and for a whole RX FIFO;
 *  the latter is what lets the space be handed back by FCFG0.RXADEAL alone,
 *  so that no FIFO pointer is written while the burst is on the air.
 *
 *  @param  numEntries  Entries the burst delivers at most
 *  @param  entryBytes  Padded length of one entry in the FIFO
 *
 *  @return %RCL_Dma_Status_Success, or %RCL_Dma_Status_Error_Param if the entry is not a power of two bytes, or the burst does not fit one transfer or the posted buffer, or the buffer cannot take a whole FIFO
 *
 *  @note This function is intended as internal to RCL and its handlers
 */
int_fast16_t RCL_Dma_armRxBurst(uint32_t numEntries, uint32_t entryBytes);

/**
 *  @brief  Drain an RX burst; to be called when the command ends
 *
 *  Accounts for what the stream moved while the burst was on the air and moves
 *  whatever is still in the RX FIFO into the posted RX buffer with one exact
 *  transfer, the PBE having ended the operation. Every
 *  entry the burst produced is then in the RX buffer, back to back and in
 *  order, starting at %firstEntry; for a burst the count returned is the
 *  bytes of the whole burst, not of one packet.
 *
 *  @param  firstEntry  Set to the first entry of the burst in the RX buffer; NULL if none landed
 *
 *  @note This function is intended as internal to RCL and its handlers
 *
 *  @return Number of bytes the whole burst committed to the RX buffer
 */
uint32_t RCL_Dma_finishRxBurst(const uint8_t **firstEntry);

/**
 *  @brief  Stop the DMA path; to be called when the command ends
 *
 *  Disables the channel and hands the posted TX burst back. The RX buffer
 *  stays posted.
 *
 *  @note This function is intended as internal to RCL and its handlers
 */
void RCL_Dma_stop(void);
/** @}
 */

/** @defgroup dmaDebugFunctions DMA Data Path Instrumentation
 *  Active in an application built with %RCL_DMA_DEBUG defined
 *  @{
 */

/**
 *  @brief  Name the GPIO the data path toggles on every FIFO request
 *
 *  The LRF DMA trigger reaches two DMA channels. The data path owns one of
 *  them; a burst arms the other on the same trigger with a transfer that
 *  writes a single word to the GPIO toggle register per arbitration. The pin
 *  is therefore driven by the DMA and not by the CPU, which is the point: an
 *  instrument the CPU had to service would be reporting on itself rather than
 *  on the data path. The data path is unchanged by this.
 *
 *  One request is answered with exactly one arbitration on every channel
 *  listening, so the pin changes state once per entry moved to or from the
 *  FIFO, in both directions.
 *
 *  Neither direction waits on data. The transfer is armed over the whole
 *  burst before the command is submitted, so an entry the application writes
 *  late still goes out on the request its position earns, and the trace looks
 *  the same as for a burst prepared in advance.
 *
 *  SysConfig allocates the second channel's control table entry when the
 *  application is compiled with %RCL_DMA_DEBUG defined, and nothing else may
 *  use that channel then. The pin solver does not know about it: another
 *  driver placed on the same channel shows up as a duplicate control table
 *  entry at link time, not as a SysConfig conflict.
 *  The RCL library itself is built the same either way: in an application
 *  without the define no channel is reserved, this call has no effect and
 *  the data path runs exactly as it does without it.
 *
 *  Call this before the first command is submitted. When the channel is
 *  allocated but no valid pin is named, the burst runs with the pin left
 *  alone and %rclDmaDebugUnavailable counts it.
 *
 *  @param  gpioIndex  GPIO driver index of an output pin, as SysConfig defines it
 */
void RCL_Dma_setDebugPin(uint_least8_t gpioIndex);

/** @}
 */

#endif /* DeviceFamily_PARENT == DeviceFamily_PARENT_CC27XX */

#endif /* ti_drivers_rcl_RCL_Dma__include */
