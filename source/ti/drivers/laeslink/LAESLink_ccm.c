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
 *  ======== LAESLink_ccm.c ========
 *  CCM block images; see LAESLink_ccm.h for the layout.
 */

#include <string.h>

#include <ti/drivers/laeslink/LAESLink_ccm.h>

/*
 *  ======== LAESLink_ccmHeader ========
 */
void LAESLink_ccmHeader(uint32_t counter, uint8_t header[LAESLINK_AAD_LEN])
{
    header[0] = (uint8_t)(counter >> 24);
    header[1] = (uint8_t)(counter >> 16);
    header[2] = (uint8_t)(counter >> 8);
    header[3] = (uint8_t)counter;
}

/*
 *  ======== LAESLink_ccmSetCounter ========
 */
void LAESLink_ccmSetCounter(LAESLink_CcmBlocks *blocks, uint32_t counter)
{
    uint8_t h[LAESLINK_AAD_LEN];
    const uint32_t w = LAESLINK_CCM_COUNTER_WORD * 4U;

    LAESLink_ccmHeader(counter, h);
    memcpy(&blocks->b0[w], h, sizeof(h));
    memcpy(&blocks->a0[w], h, sizeof(h));
    memcpy(&blocks->a1[w], h, sizeof(h));
    memcpy(&blocks->b1[LAESLINK_CCM_B1_COUNTER_OFFSET], h, sizeof(h));
}

/*
 *  ======== LAESLink_ccmBlocks ========
 */
void LAESLink_ccmBlocks(LAESLink_CcmBlocks *blocks, const uint8_t sid[3], const uint8_t tail[6], uint32_t counter)
{
    uint8_t nonceBlock[16] = {0};

    memcpy(&nonceBlock[1], sid, 3);
    memcpy(&nonceBlock[8], tail, 6);

    memcpy(blocks->b0, nonceBlock, sizeof(nonceBlock));
    blocks->b0[0]  = LAESLINK_CCM_B0_FLAGS;
    blocks->b0[14] = 0U;
    blocks->b0[15] = LAESLINK_PAYLOAD_LEN;

    memcpy(blocks->a0, nonceBlock, sizeof(nonceBlock));
    blocks->a0[0] = LAESLINK_CCM_A_FLAGS;

    memcpy(blocks->a1, blocks->a0, sizeof(blocks->a1));
    blocks->a1[15] = 1U;

    memset(blocks->b1, 0, sizeof(blocks->b1));
    blocks->b1[1] = LAESLINK_AAD_LEN;

    LAESLink_ccmSetCounter(blocks, counter);
}

/*
 *  ======== LAESLink_blockToWords ========
 */
void LAESLink_blockToWords(const uint8_t block[16], uint32_t words[4])
{
    for (uint32_t i = 0U; i < 4U; i++)
    {
        words[i] = (uint32_t)block[4U * i] | ((uint32_t)block[4U * i + 1U] << 8) |
                   ((uint32_t)block[4U * i + 2U] << 16) | ((uint32_t)block[4U * i + 3U] << 24);
    }
}
