/*
 * Copyright (c) 2019-2023, Texas Instruments Incorporated
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
/** ============================================================================
 *  @file       CryptoUtils.h
 *
 *  @brief      A collection of utility functions for cryptographic purposes
 *
 */

#ifndef ti_drivers_cryptoutils_utils_CryptoUtils__include
#define ti_drivers_cryptoutils_utils_CryptoUtils__include

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 *  @brief Indicates the endianess (byte order) of a multi-byte value.
 */
typedef enum
{
    CryptoUtils_ENDIANESS_BIG    = 0u, /*!< MSB at lowest address. */
    CryptoUtils_ENDIANESS_LITTLE = 1u, /*!< LSB at highest address. */
} CryptoUtils_Endianess;

/*!
 *  @brief Limit value of 0
 *
 *  This is a value provided for convenience when checking a value
 *  against a range.
 *
 *  @sa     CryptoUtils_limitOne
 *  @sa     CryptoUtils_isNumberInRange
 */
extern const uint8_t *CryptoUtils_limitZero;

/*!
 *  @brief Limit value of 1
 *
 *  This is a value provided for convenience when checking a value
 *  against a range.
 *
 *  @sa     CryptoUtils_limitZero
 *  @sa     CryptoUtils_isNumberInRange
 */
extern const uint8_t *CryptoUtils_limitOne;

/**
 *  @brief Compares two buffers for equality without branching
 *
 *  @note   This is not a drop-in replacement for memcmp!
 *
 *  Most memcmp implementations break out of their comparison loop immediately
 *  once a mismatch is detected to save execution time. For cryptographic
 *  purposes, this is a flaw.
 *
 *  This function compares two buffers without branching thus requiring a
 *  an amount of time that does not vary with the content of @c buffer0 and
 *  @c buffer1.
 *
 *  @param  buffer0             Buffer to compare against @c buffer1.
 *  @param  buffer1             Buffer tp compare against @c buffer0
 *  @param  bufferByteLength    Length in bytes of @c buffer0 and @c buffer1.
 *  @retval true                The contents of the buffers match.
 *  @retval false               The contents of the buffers do not match.
 */
bool CryptoUtils_buffersMatch(const volatile void *volatile buffer0,
                              const volatile void *volatile buffer1,
                              size_t bufferByteLength);

/**
 *  @brief Compares two buffers for equality word-by-word without branching
 *
 *  @note   This is not a drop-in replacement for memcmp!
 *
 *  Most memcmp implementations break out of their comparison loop immediately
 *  once a mismatch is detected to save execution time. For cryptographic
 *  purposes, this is a flaw.
 *
 *  This function compares two buffers without branching thus requiring a
 *  an amount of time that does not vary with the content of @c buffer0 and
 *  @c buffer1.
 *
 *  Unlike #CryptoUtils_buffersMatch(), this function expects @c buffer0 and
 *  @c buffer1 to be 32-bit aligned. It will only perform 32-bit aligned
 *  accesses to memory. This is needed to access the registers of certain
 *  peripherals.
 *
 *  @param  buffer0             Buffer to compare against @c buffer1.
 *  @param  buffer1             Buffer tp compare against @c buffer0
 *  @param  bufferByteLength    Length in bytes of @c buffer0 and @c buffer1.
 *                              Must be evenly divisible by sizeof(uint32_t).
 *                              This function will return false if @c
 *                              bufferByteLength is not evenly divisible by
 *                              sizeof(uin32_t).
 *  @retval true                The contents of the buffers match.
 *  @retval false               The contents of the buffers do not match.
 */
bool CryptoUtils_buffersMatchWordAligned(const volatile uint32_t *volatile buffer0,
                                         const volatile uint32_t *volatile buffer1,
                                         size_t bufferByteLength);

/**
 *  @brief Check whether the provided buffer only contains 0x00 bytes
 *
 *  @param  buffer              Buffer to search for non-zero bytes
 *  @param  bufferByteLength    Length of @c buffer in bytes
 *
 *  @retval true                The buffer contained only bytes with value 0x00
 *  @retval false               The buffer contained at least on non-zero byte
 */
bool CryptoUtils_isBufferAllZeros(const void *buffer, size_t bufferByteLength);

/**
 *  @brief  Copies @c val into the first @c count bytes of the buffer
 *          pointed to by @c dest.
 *
 *  @param  dest                Pointer to destination buffer
 *  @param  destSize            Size of destination buffer in bytes
 *  @param  val                 Fill byte value
 *  @param  count               Number of bytes to fill
 *
 */
void CryptoUtils_memset(void *dest, size_t destSize, uint8_t val, size_t count);

/**
 *  @brief Reverses the byte order in a buffer of a given length
 *
 *  The left-most byte will become the right-most byte and vice versa.
 *
 *  @param  buffer              Buffer containing the data to be reversed.
 *  @param  bufferByteLength    Length in bytes of @c buffer.
 */
void CryptoUtils_reverseBufferBytewise(void *buffer, size_t bufferByteLength);

/**
 *  @brief Copies and pads an array of words.
 *
 *  The \c source array is copied into the \c destination
 *  array. Writes are done word-wise. If \c sourceLength is not a multiple of 4,
 *  any remaining bytes up to the next word boundary are padded with 0.
 *
 *  The length of the destination array must be a multiple of 4, rounded up to the
 *  padded \c sourceLength if required.
 *
 *  @param source       Source array
 *
 *  @param destination  Destination array
 *
 *  @param sourceLength Length of the source array
 */
void CryptoUtils_copyPad(const void *source, uint32_t *destination, size_t sourceLength);

/**
 *  @brief Reverses, copies, and pads an array of words.
 *
 *  The \c source array is reversed byte-wise and copied into the \c destination
 *  array. Writes are done word-wise. If \c sourceLength is not a multiple of 4,
 *  any remaining bytes up to the next word boundary are padded with 0.
 *
 *  The length of the destination array must be a multiple of 4, rounded up to the
 *  padded \c sourceLength if required.
 *
 *  @param source       Source array
 *
 *  @param destination  Destination array
 *
 *  @param sourceLength Length of the source array
 */
void CryptoUtils_reverseCopyPad(const void *source, uint32_t *destination, size_t sourceLength);

/**
 *  @brief Reverses and copies an array of bytes.
 *
 *  The \c source array is reversed byte-wise and copied into the \c destination
 *  array.
 *
 *  @param source       Source array
 *
 *  @param destination  Destination array
 *
 *  @param sourceLength Length of the source array
 */
void CryptoUtils_reverseCopy(const void *source, void *destination, size_t sourceLength);

/**
 *  @brief Checks if number is within the range [lowerLimit, upperLimit)
 *
 *  Checks if the specified number is at greater than or equal to
 *  the lower limit and less than the upper limit. Note that the boundary
 *  set by the upper limit is not inclusive.
 *
 *  Note that the special values of #CryptoUtils_limitZero and
 *  #CryptoUtils_limitOne are available to pass in for the @c lowerLimit.
 *  (These values can also be used for the @c upperLimit but their use for
 *  the upperLimit has no practical use.)
 *
 *  If @c lowerLimit is NULL then the lower limit is taken as 0.
 *  If @c upperLimit is NULL then the upper limit is taken as
 *  2<sup>(@c bitLength + 1)</sup>.
 *
 *  The implemented algorithm is timing-constant when the following parameters
 *  are held constant: @c lowerLimit, @c upperLimit, @c bitLength,
 *  and @c endianess. Thus, the @c number being checked may change and
 *  timing will not leak its relation to the limits. However, timing may leak
 *  the bitLength, the endianess, and the use of #CryptoUtils_limitZero,
 *  #CryptoUtils_limitOne, and NULL for the limit values.
 *
 *  @param  number              Pointer to number to check
 *  @param  bitLength           Length in bits of @c number, @c lowerLimit, and
 *                              @c upperLimit.
 *  @param  endianess           The endianess of @c number, @c lowerLimit, and
 *                              @c upperLimit.
 *  @param  lowerLimit          Pointer to lower limit value.
 *  @param  upperLimit          Pointer to upper limit value.
 *  @retval true                The randomNumber is within
 *                              [@c lowerLimit, @c upperLimit).
 *  @retval false               The randomNumber is not within
 *                              [@c lowerLimit, @c upperLimit).
 */
bool CryptoUtils_isNumberInRange(const void *number,
                                 size_t bitLength,
                                 CryptoUtils_Endianess endianess,
                                 const void *lowerLimit,
                                 const void *upperLimit);

#ifdef __cplusplus
}
#endif

#endif /* ti_drivers_cryptoutils_utils_CryptoUtils__include */
