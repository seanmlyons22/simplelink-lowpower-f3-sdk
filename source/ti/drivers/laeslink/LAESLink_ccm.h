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
 *  ======== LAESLink_ccm.h ========
 *  AES-CCM block formatting for the fixed link packet.
 *
 *  RFC 3610 / NIST SP 800-38C with L = 2 (13-byte nonce, two-byte length
 *  field). Nonce layout and the CCM blocks built from it:
 *
 *    octet:   0     1..3      4..7        8..13      14  15
 *    B0:   0x49 | sid[3] | pc[4] BE | tail[6] | 0x00 0x10     Adata | M'=1 | L'=1 ; l(m)=16
 *    A0:   0x01 | sid[3] | pc[4] BE | tail[6] | 0x00 0x00     L'=1 ; i=0
 *    A1:   0x01 | sid[3] | pc[4] BE | tail[6] | 0x00 0x01     i=1
 *    B1:   0x00 0x04 | pc[4] BE (octets 2..5) | zeros(10)     l(a)=4 ; AAD = header = pc
 *
 *  pc is the 32-bit packet counter, big endian. It is word 1 of B0, A0 and A1
 *  and also the on-air header. That placement is what lets the LAES counter
 *  hardware (CTR64, left aligned, big endian) advance it, and lets a single
 *  DMA word copy distribute it.
 */

#ifndef ti_drivers_laeslink_LAESLink_ccm__include
#define ti_drivers_laeslink_LAESLink_ccm__include

#include <stdint.h>

#include <ti/drivers/laeslink/LAESLink.h>

#ifdef __cplusplus
extern "C" {
#endif

/* B0 flags: Adata | M' = (4 - 2) / 2 | L' = 2 - 1 */
#define LAESLINK_CCM_B0_FLAGS           (0x40U | (((LAESLINK_MIC_LEN - 2U) / 2U) << 3) | 0x01U)
/* A_i flags: L' = 1 */
#define LAESLINK_CCM_A_FLAGS            (0x01U)
/* Word index of the packet counter inside B0, A0 and A1 */
#define LAESLINK_CCM_COUNTER_WORD       (1U)
/* Byte offset of the packet counter inside B1 */
#define LAESLINK_CCM_B1_COUNTER_OFFSET  (2U)

_Static_assert(LAESLINK_CCM_B0_FLAGS == 0x49U, "B0 flags for M = 4, L = 2, with AAD");

/* The four CCM blocks for one packet counter value, in memory order */
typedef struct
{
    uint8_t b0[16];  /* CBC-MAC first block */
    uint8_t b1[16];  /* CBC-MAC AAD block */
    uint8_t a0[16];  /* CTR block, counter 0 (MIC keystream) */
    uint8_t a1[16];  /* CTR block, counter 1 (payload keystream) */
} LAESLink_CcmBlocks;

/* Build the blocks for counter */
void LAESLink_ccmBlocks(LAESLink_CcmBlocks *blocks, const uint8_t sid[3], const uint8_t tail[6], uint32_t counter);

/* Store counter into every block */
void LAESLink_ccmSetCounter(LAESLink_CcmBlocks *blocks, uint32_t counter);

/* The on-air header for counter: the counter, big endian */
void LAESLink_ccmHeader(uint32_t counter, uint8_t header[LAESLINK_AAD_LEN]);

/* A 16-byte block as the four little-endian words the LAES data registers take */
void LAESLink_blockToWords(const uint8_t block[16], uint32_t words[4]);

#ifdef __cplusplus
}
#endif

#endif /* ti_drivers_laeslink_LAESLink_ccm__include */
