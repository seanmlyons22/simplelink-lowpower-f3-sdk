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
 *  ======== LAESLink_udma.h ========
 *  PL230 channel control words and scatter-gather task images.
 *
 *  The link builds its control words itself rather than through
 *  uDMATaskStructEntry: that macro forces ALT_SELECT onto every scatter-gather
 *  task, and the lists need the mixed b101/b111 cycle types that make a task
 *  either continue on its own or wait for the next request.
 *
 *  Control word layout (PL230 TRM table 2-16):
 *
 *    [31:30] DSTINC  [29:28] DSTSIZE  [27:26] SRCINC  [25:24] SRCSIZE
 *    [23:18] HPROT   [17:14] ARB (2^R) [13:4] XFERSIZE-1 [3] NEXTUSEBURST [2:0] MODE
 */

#ifndef ti_drivers_laeslink_LAESLink_udma__include
#define ti_drivers_laeslink_LAESLink_udma__include

#include <stdbool.h>
#include <stdint.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/udma.h)

#ifdef __cplusplus
extern "C" {
#endif

/* Address increment per item */
#define LAESLINK_INC_BYTE   (0U)
#define LAESLINK_INC_HALF   (1U)
#define LAESLINK_INC_WORD   (2U)
#define LAESLINK_INC_NONE   (3U)

/* Item size. Source and destination size are always equal on the PL230 */
#define LAESLINK_SIZE_BYTE  (0U)
#define LAESLINK_SIZE_HALF  (1U)
#define LAESLINK_SIZE_WORD  (2U)

/* Arbitration interval: items moved before the controller re-arbitrates */
#define LAESLINK_ARB_X1     (0U)
#define LAESLINK_ARB_X2     (1U)
#define LAESLINK_ARB_X4     (2U)
#define LAESLINK_ARB_X16    (4U)

/* Cycle type. A memory scatter-gather task (MEM_SG_ALT) auto-requests, so the
 * list continues into the next task with no request from outside. A
 * peripheral scatter-gather task (PER_SG_ALT) stops the list until the next
 * request, which is how the list waits for the LAES.
 */
#define LAESLINK_MODE_BASIC       (1U)
#define LAESLINK_MODE_AUTO        (2U)
#define LAESLINK_MODE_MEM_SG      (4U)
#define LAESLINK_MODE_MEM_SG_ALT  (5U)
#define LAESLINK_MODE_PER_SG      (6U)
#define LAESLINK_MODE_PER_SG_ALT  (7U)

/* Largest transfer count a control word can hold */
#define LAESLINK_MAX_ITEMS  (1024U)

/* Encode a control word. n is the item count, 1 to 1024. */
#define LAESLINK_CONTROL_WORD(dstInc, srcInc, size, arb, n, mode) \
    (((uint32_t)(dstInc) << 30) | ((uint32_t)(size) << 28) | ((uint32_t)(srcInc) << 26) | \
     ((uint32_t)(size) << 24) | ((uint32_t)(arb) << 14) | (((uint32_t)(n) - 1U) << 4) | (uint32_t)(mode))

/* The four control words the design document names, pinned here so that a
 * change to the encoder cannot go unnoticed.
 */
_Static_assert(LAESLINK_CONTROL_WORD(LAESLINK_INC_WORD, LAESLINK_INC_WORD, LAESLINK_SIZE_WORD, LAESLINK_ARB_X4, 4U,
                                     LAESLINK_MODE_PER_SG_ALT) == 0xAA008037U,
               "W4I: four words, both incrementing");
_Static_assert(LAESLINK_CONTROL_WORD(LAESLINK_INC_NONE, LAESLINK_INC_WORD, LAESLINK_SIZE_WORD, LAESLINK_ARB_X4, 4U,
                                     LAESLINK_MODE_PER_SG_ALT) == 0xEA008037U,
               "W4F: four words into a fixed port");
_Static_assert(LAESLINK_CONTROL_WORD(LAESLINK_INC_NONE, LAESLINK_INC_NONE, LAESLINK_SIZE_WORD, LAESLINK_ARB_X1, 1U,
                                     LAESLINK_MODE_PER_SG_ALT) == 0xEE000007U,
               "W1: one word, fixed addresses");
_Static_assert(LAESLINK_CONTROL_WORD(LAESLINK_INC_HALF, LAESLINK_INC_HALF, LAESLINK_SIZE_HALF, LAESLINK_ARB_X2, 2U,
                                     LAESLINK_MODE_PER_SG_ALT) == 0x55004017U,
               "H2: two halfwords, both incrementing");

/* One channel control structure image: a scatter-gather task, or the content
 * of a control table entry. The layout is the hardware's; the alignment keeps
 * a list of them on 16-byte boundaries.
 */
typedef struct
{
    uint32_t srcEnd;  /* Inclusive source end pointer */
    uint32_t dstEnd;  /* Inclusive destination end pointer */
    uint32_t control; /* Control word */
    uint32_t spare;   /* Unused by the hardware. Zero. */
} __attribute__((aligned(16))) LAESLink_Task;

/* Bytes added to the address per item, or zero for a fixed address */
static inline uint32_t LAESLink_incBytes(uint32_t inc)
{
    return (inc == LAESLINK_INC_NONE) ? 0U : (1U << inc);
}

/* Inclusive end address for a transfer of n items that starts at start. The
 * PL230 derives the current address as end - remaining * increment, so the
 * end pointer is the address of the last item, or the register address for a
 * fixed pointer.
 */
static inline uint32_t LAESLink_endAddress(uint32_t start, uint32_t n, uint32_t inc)
{
    return start + (n - 1U) * LAESLink_incBytes(inc);
}

/* Build a task from start addresses, one parameter per control word field */
static inline LAESLink_Task LAESLink_transfer(uint32_t src,
                                              uint32_t dst,
                                              uint32_t n,
                                              uint32_t size,
                                              uint32_t srcInc,
                                              uint32_t dstInc,
                                              uint32_t arb,
                                              uint32_t mode)
{
    LAESLink_Task t;

    t.srcEnd  = LAESLink_endAddress(src, n, srcInc);
    t.dstEnd  = LAESLink_endAddress(dst, n, dstInc);
    t.control = LAESLINK_CONTROL_WORD(dstInc, srcInc, size, arb, n, mode);
    t.spare   = 0U;
    return t;
}

/* Item count encoded in a control word */
static inline uint32_t LAESLink_taskItems(uint32_t control)
{
    return ((control >> 4) & 0x3FFU) + 1U;
}

/* Same task with a different cycle type */
static inline LAESLink_Task LAESLink_withMode(LAESLink_Task t, uint32_t mode)
{
    t.control = (t.control & ~UDMA_MODE_M) | mode;
    return t;
}

/* Primary control structure for a scatter-gather list of tasks entries. The
 * primary copies four words per task from the list into the channel's
 * alternate structure. lastTaskSpare is the address of the spare word of the
 * last list entry; altSpare is the address of the alternate structure's spare
 * word.
 */
static inline LAESLink_Task LAESLink_sgPrimary(uint32_t lastTaskSpare, uint32_t altSpare, uint32_t tasks, bool peripheral)
{
    LAESLink_Task t;

    t.srcEnd  = lastTaskSpare;
    t.dstEnd  = altSpare;
    t.control = LAESLINK_CONTROL_WORD(LAESLINK_INC_WORD, LAESLINK_INC_WORD, LAESLINK_SIZE_WORD, LAESLINK_ARB_X4,
                                      tasks * 4U, peripheral ? LAESLINK_MODE_PER_SG : LAESLINK_MODE_MEM_SG);
    t.spare   = 0U;
    return t;
}

/* Address of the spare word of a task (the inclusive end of a 4-word copy) */
static inline uint32_t LAESLink_taskSpareAddr(const volatile LAESLink_Task *t)
{
    return (uint32_t)&t->spare;
}

/* Program a control table entry from a task image. The control word goes
 * last: a non-zero mode is what makes the entry live.
 */
static inline void LAESLink_writeEntry(volatile uDMAControlTableEntry *e, LAESLink_Task t)
{
    e->pSrcEndAddr = (volatile void *)t.srcEnd;
    e->pDstEndAddr = (volatile void *)t.dstEnd;
    e->spare       = t.spare;
    e->control     = t.control;
}

/* Invalidate a control table entry (stop mode) */
static inline void LAESLink_clearEntry(volatile uDMAControlTableEntry *e)
{
    e->pSrcEndAddr = (volatile void *)0;
    e->pDstEndAddr = (volatile void *)0;
    e->spare       = 0U;
    e->control     = 0U;
}

/* Address of an entry's first word, spare word and control word, for the tasks
 * that refresh a primary or re-arm a channel.
 */
static inline uint32_t LAESLink_entryStartAddr(const volatile uDMAControlTableEntry *e)
{
    return (uint32_t)e;
}

static inline uint32_t LAESLink_entrySpareAddr(const volatile uDMAControlTableEntry *e)
{
    return (uint32_t)&e->spare;
}

#ifdef __cplusplus
}
#endif

#endif /* ti_drivers_laeslink_LAESLink_udma__include */
