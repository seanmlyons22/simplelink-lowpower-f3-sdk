// ===========================================================================
// This file is autogenerated, please DO NOT modify!
//
// Generated on  2023-04-18 13:27:20
// by user:      developer
// on machine:   swtools
// CWD:          /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tc/octopus/regs/inc
// Commandline:  /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tools/topsm/topsmregs.pl -target c -base 0x40081C00 -dat_sz 32 -i /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tc/octopus/regs/doc/RXFregs.txt
// C&P friendly: /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tools/topsm/topsmregs.pl -target c -base 0x40081C00 -dat_sz 32 -i /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tc/octopus/regs/doc/RXFregs.txt
//
// Relevant file version(s):
//
// /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tools/topsm/topsmregs.pl
//   rcs-info: (file not managed or unknown revision control system)
//   git-hash: 0d11a0ea4ba55ba3ef648be18aeec231d5314753
//
// /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tc/octopus/regs/doc/RXFregs.txt
//   rcs-info: (file not managed or unknown revision control system)
//   git-hash: 2406be3bc1765b60a0cd63501c816046956d5a0b
//
// ===========================================================================


#ifndef __RXF_REGS_H
#define __RXF_REGS_H

#ifndef __HW_TYPES_H__ 
  #ifndef HWREG
    #define HWREG(x) (*((volatile unsigned long *)(x)))
  #endif
#endif
#define RXF_BASE 0x40081C00UL
#define RF24_RXF_BASE 0x40081C00UL
// --------------------------------------------------------------
// RXD
// 
#define RXF_RXD_ADR (RXF_BASE + 0x0000UL)
static volatile unsigned long* const SP_RXF_RXD = (unsigned long*) RXF_RXD_ADR;
#define S_RXF_RXD (*SP_RXF_RXD)
#define RF24_RXF_O_RXD                           0
// bitfield: RXD_DATA
#define RXF_RXD_DATA                                 0UL
#define RF24_RXF_RXD_DATA_S                      0UL
#define RXF_RXD_DATA_BM                              0xFFFFFFFFUL
#define RF24_RXF_RXD_DATA_M                      0xFFFFFFFFUL

#endif