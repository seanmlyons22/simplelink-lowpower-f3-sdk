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
 *  ======== LAESLink.c ========
 *  What the transmit and receive sessions share: the control table entries
 *  of the fabric channels, power, the LAES setup and the channel housekeeping.
 */

#include <string.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_aes.h)
#include DeviceFamily_constructPath(inc/hw_dma.h)
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(driverlib/aes.h)
#include DeviceFamily_constructPath(driverlib/interrupt.h)
#include DeviceFamily_constructPath(driverlib/udma.h)

#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC27XX.h>
#include <ti/drivers/dma/UDMALPF3.h>
#include <ti/drivers/laeslink/LAESLink_lists.h>

/* Channels 8, 9 and 10 and their alternates (the entry 16 above the primary,
 * which scatter-gather needs and nothing allocates by default). A second
 * allocation of the same entry anywhere in the image fails at link time,
 * which is the guard we want.
 */
ALLOCATE_CONTROL_TABLE_ENTRY(LAESLink_ch8Primary, 8U);
ALLOCATE_CONTROL_TABLE_ENTRY(LAESLink_ch8Alternate, 8U | UDMA_ALT_SELECT);
ALLOCATE_CONTROL_TABLE_ENTRY(LAESLink_ch9Primary, 9U);
ALLOCATE_CONTROL_TABLE_ENTRY(LAESLink_ch9Alternate, 9U | UDMA_ALT_SELECT);
ALLOCATE_CONTROL_TABLE_ENTRY(LAESLink_ch10Primary, 10U);
ALLOCATE_CONTROL_TABLE_ENTRY(LAESLink_ch10Alternate, 10U | UDMA_ALT_SELECT);

/*
 *  ======== LAESLink_primary ========
 */
volatile uDMAControlTableEntry *LAESLink_primary(uint32_t channel)
{
    switch (channel)
    {
        case 8U:
            return &LAESLink_ch8Primary;
        case 9U:
            return &LAESLink_ch9Primary;
        case 10U:
            return &LAESLink_ch10Primary;
        default:
            /* A peripheral channel: its entry is allocated by the owning
             * driver's SysConfig template at this same slot of the table.
             */
            return (volatile uDMAControlTableEntry *)UDMALPF3_config.CtrlBaseAddr + channel;
    }
}

/*
 *  ======== LAESLink_alternate ========
 */
volatile uDMAControlTableEntry *LAESLink_alternate(uint32_t channel)
{
    switch (channel)
    {
        case 8U:
            return &LAESLink_ch8Alternate;
        case 9U:
            return &LAESLink_ch9Alternate;
        default:
            return &LAESLink_ch10Alternate;
    }
}

/*
 *  ======== LAESLink_publish ========
 */
void LAESLink_publish(void)
{
    __asm volatile("" ::: "memory");
    __DSB();
}

/*
 *  ======== LAESLink_open ========
 */
void LAESLink_open(void)
{
    Power_setDependency(PowerLPF3_PERIPH_DMA);
    Power_setDependency(PowerLPF3_PERIPH_AES);
    /* A detached pipeline must not be cut off by a standby entry, which
     * would lose both uDMA and LAES state.
     */
    Power_setConstraint(PowerLPF3_DISALLOW_STANDBY);
    UDMALPF3_init();
}

/*
 *  ======== LAESLink_close ========
 */
void LAESLink_close(void)
{
    Power_releaseConstraint(PowerLPF3_DISALLOW_STANDBY);
    Power_releaseDependency(PowerLPF3_PERIPH_AES);
    Power_releaseDependency(PowerLPF3_PERIPH_DMA);
}

/*
 *  ======== LAESLink_keyValid ========
 */
bool LAESLink_keyValid(void)
{
    uint32_t sta = HWREG(AES_BASE + AES_O_STA);

    return ((sta & AES_STA_KEYSTATE_M) == AES_STA_KEYSTATE_VALID) && ((sta & AES_STA_KEYINTID_M) == AES_STA_KEYINTID_CORE);
}

/*
 *  ======== LAESLink_laesOpen ========
 */
bool LAESLink_laesOpen(const uint8_t key[LAESLINK_KEY_LEN], uint32_t autocfg)
{
    /* ABORT clears TXT, BUF, DMA and AUTOCFG, so this is a full reset */
    AESAbort();
    AESDisableDMA();
    AESClearInterrupt(AES_ICLR_ALL);
    /* All four key words come from one initiator, which is what
     * STA.KEYSTATE requires.
     */
    AESWriteKEY(key);
    if (!LAESLink_keyValid())
    {
        return false;
    }
    /* AESDONE alone reaches MIS, and so the AES_COMB event that paces the
     * lists. The interrupt line itself stays disabled in the NVIC.
     */
    AESSetIMASK(AES_IMASK_AESDONE);
    AESSetAUTOCFG(autocfg);
    return true;
}

/*
 *  ======== LAESLink_laesClose ========
 */
void LAESLink_laesClose(void)
{
    static const uint8_t zeroKey[LAESLINK_KEY_LEN] = {0};

    AESSetIMASK(0U);
    AESAbort();
    AESClearInterrupt(AES_ICLR_ALL);
    AESWriteKEY(zeroKey);
}

/*
 *  ======== LAESLink_channelsReset ========
 */
void LAESLink_channelsReset(uint32_t mask)
{
    uDMADisableChannel(mask);
    uDMADisableChannelAttribute(mask, UDMA_ATTR_ALL);
    HWREG(DMA_BASE + DMA_O_DONEMASK) &= ~mask;
    uDMAClearInt(mask);
}

/*
 *  ======== LAESLink_doneMaskSet ========
 */
void LAESLink_doneMaskSet(uint32_t mask)
{
    HWREG(DMA_BASE + DMA_O_DONEMASK) |= mask;
}

/*
 *  ======== LAESLink_takeDmaError ========
 */
bool LAESLink_takeDmaError(void)
{
    if (uDMAGetErrorStatus() != 0U)
    {
        uDMAClearErrorStatus();
        return true;
    }
    return false;
}
