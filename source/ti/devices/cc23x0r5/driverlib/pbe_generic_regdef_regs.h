// ===========================================================================
// This file is autogenerated, please DO NOT modify!
//
// Generated on  2023-04-18 13:27:21
// by user:      developer
// on machine:   swtools
// CWD:          /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tc/octopus/regs/inc
// Commandline:  /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tools/topsm/topsmregs.pl -target c -base 0x40092000 -dat_sz 16 -sub 128 -ramreg -i /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/pbe/generic/doc/regdef.txt
// C&P friendly: /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tools/topsm/topsmregs.pl -target c -base 0x40092000 -dat_sz 16 -sub 128 -ramreg -i /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/pbe/generic/doc/regdef.txt
//
// Relevant file version(s):
//
// /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/tools/topsm/topsmregs.pl
//   rcs-info: (file not managed or unknown revision control system)
//   git-hash: 0d11a0ea4ba55ba3ef648be18aeec231d5314753
//
// /home/developer/.conan/data/loki-lrf/7.11.00.18/library-lprf/eng/build/7c92806c54e4cfdd9df8e8c53e37468130adda90/pbe/generic/doc/regdef.txt
//   rcs-info: (file not managed or unknown revision control system)
//   git-hash: 805fda6e271b22cce38ca9688119ac493be2f73e
//
// ===========================================================================


#ifndef __PBE_GENERIC_REGDEF_REGS_H
#define __PBE_GENERIC_REGDEF_REGS_H

#define PBE_GENERIC_REGDEF_BASE 0x40092000UL
// --------------------------------------------------------------
// IRQ
// 
#define PBE_GENERIC_REGDEF_IRQ_ADR (PBE_GENERIC_REGDEF_BASE + 0xFFFFFFFFFFFFFF08UL)
static volatile unsigned short* const SP_PBE_GENERIC_REGDEF_IRQ = (unsigned short*) PBE_GENERIC_REGDEF_IRQ_ADR;
#define S_PBE_GENERIC_REGDEF_IRQ (*SP_PBE_GENERIC_REGDEF_IRQ)
// bitfield: IRQ_OPERROR
#define PBE_GENERIC_REGDEF_IRQ_OPERROR                              15UL
#define PBE_GENERIC_REGDEF_IRQ_OPERROR_BM                           0x8000UL
// enums for bitfield IRQ_OPERROR (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_OPERROR_OFF                          0x0UL
#define PBE_GENERIC_REGDEF_IRQ_OPERROR_ACTIVE                       0x1UL
// bitfield: IRQ_UNUSED14
#define PBE_GENERIC_REGDEF_IRQ_UNUSED14                             14UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED14_BM                          0x4000UL
// enums for bitfield IRQ_UNUSED14 (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED14_OFF                         0x0UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED14_ACTIVE                      0x1UL
// bitfield: IRQ_TXDONE
#define PBE_GENERIC_REGDEF_IRQ_TXDONE                               13UL
#define PBE_GENERIC_REGDEF_IRQ_TXDONE_BM                            0x2000UL
// enums for bitfield IRQ_TXDONE (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_TXDONE_OFF                           0x0UL
#define PBE_GENERIC_REGDEF_IRQ_TXDONE_ACTIVE                        0x1UL
// bitfield: IRQ_UNUSED12
#define PBE_GENERIC_REGDEF_IRQ_UNUSED12                             12UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED12_BM                          0x1000UL
// enums for bitfield IRQ_UNUSED12 (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED12_OFF                         0x0UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED12_ACTIVE                      0x1UL
// bitfield: IRQ_UNUSED11
#define PBE_GENERIC_REGDEF_IRQ_UNUSED11                             11UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED11_BM                          0x0800UL
// enums for bitfield IRQ_UNUSED11 (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED11_OFF                         0x0UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED11_ACTIVE                      0x1UL
// bitfield: IRQ_UNUSED10
#define PBE_GENERIC_REGDEF_IRQ_UNUSED10                             10UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED10_BM                          0x0400UL
// enums for bitfield IRQ_UNUSED10 (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED10_OFF                         0x0UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED10_ACTIVE                      0x1UL
// bitfield: IRQ_UNUSED9
#define PBE_GENERIC_REGDEF_IRQ_UNUSED9                              9UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED9_BM                           0x0200UL
// enums for bitfield IRQ_UNUSED9 (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED9_OFF                          0x0UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED9_ACTIVE                       0x1UL
// bitfield: IRQ_RXOK
#define PBE_GENERIC_REGDEF_IRQ_RXOK                                 8UL
#define PBE_GENERIC_REGDEF_IRQ_RXOK_BM                              0x0100UL
// enums for bitfield IRQ_RXOK (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_RXOK_OFF                             0x0UL
#define PBE_GENERIC_REGDEF_IRQ_RXOK_ACTIVE                          0x1UL
// bitfield: IRQ_RXFOVFL
#define PBE_GENERIC_REGDEF_IRQ_RXFOVFL                              7UL
#define PBE_GENERIC_REGDEF_IRQ_RXFOVFL_BM                           0x0080UL
// enums for bitfield IRQ_RXFOVFL (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_RXFOVFL_OFF                          0x0UL
#define PBE_GENERIC_REGDEF_IRQ_RXFOVFL_ACTIVE                       0x1UL
// bitfield: IRQ_UNUSED6
#define PBE_GENERIC_REGDEF_IRQ_UNUSED6                              6UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED6_BM                           0x0040UL
// enums for bitfield IRQ_UNUSED6 (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED6_OFF                          0x0UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED6_ACTIVE                       0x1UL
// bitfield: IRQ_RXIGN
#define PBE_GENERIC_REGDEF_IRQ_RXIGN                                5UL
#define PBE_GENERIC_REGDEF_IRQ_RXIGN_BM                             0x0020UL
// enums for bitfield IRQ_RXIGN (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_RXIGN_OFF                            0x0UL
#define PBE_GENERIC_REGDEF_IRQ_RXIGN_ACTIVE                         0x1UL
// bitfield: IRQ_RXNOK
#define PBE_GENERIC_REGDEF_IRQ_RXNOK                                4UL
#define PBE_GENERIC_REGDEF_IRQ_RXNOK_BM                             0x0010UL
// enums for bitfield IRQ_RXNOK (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_RXNOK_OFF                            0x0UL
#define PBE_GENERIC_REGDEF_IRQ_RXNOK_ACTIVE                         0x1UL
// bitfield: IRQ_UNUSED3
#define PBE_GENERIC_REGDEF_IRQ_UNUSED3                              3UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED3_BM                           0x0008UL
// enums for bitfield IRQ_UNUSED3 (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED3_OFF                          0x0UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED3_ACTIVE                       0x1UL
// bitfield: IRQ_UNUSED2
#define PBE_GENERIC_REGDEF_IRQ_UNUSED2                              2UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED2_BM                           0x0004UL
// enums for bitfield IRQ_UNUSED2 (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED2_OFF                          0x0UL
#define PBE_GENERIC_REGDEF_IRQ_UNUSED2_ACTIVE                       0x1UL
// bitfield: IRQ_PINGRSP
#define PBE_GENERIC_REGDEF_IRQ_PINGRSP                              1UL
#define PBE_GENERIC_REGDEF_IRQ_PINGRSP_BM                           0x0002UL
// enums for bitfield IRQ_PINGRSP (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_PINGRSP_OFF                          0x0UL
#define PBE_GENERIC_REGDEF_IRQ_PINGRSP_ACTIVE                       0x1UL
// bitfield: IRQ_OPDONE
#define PBE_GENERIC_REGDEF_IRQ_OPDONE                               0UL
#define PBE_GENERIC_REGDEF_IRQ_OPDONE_BM                            0x0001UL
// enums for bitfield IRQ_OPDONE (width: 1)UL
#define PBE_GENERIC_REGDEF_IRQ_OPDONE_OFF                           0x0UL
#define PBE_GENERIC_REGDEF_IRQ_OPDONE_ACTIVE                        0x1UL
// --------------------------------------------------------------
// API
// 
#define PBE_GENERIC_REGDEF_API_ADR (PBE_GENERIC_REGDEF_BASE + 0xFFFFFFFFFFFFFF18UL)
static volatile unsigned short* const SP_PBE_GENERIC_REGDEF_API = (unsigned short*) PBE_GENERIC_REGDEF_API_ADR;
#define S_PBE_GENERIC_REGDEF_API (*SP_PBE_GENERIC_REGDEF_API)
// bitfield: API_OP
#define PBE_GENERIC_REGDEF_API_OP                                   0UL
#define PBE_GENERIC_REGDEF_API_OP_BM                                0x001FUL
// enums for bitfield API_OP (width: 5)UL
#define PBE_GENERIC_REGDEF_API_OP_PING                              0x00UL
#define PBE_GENERIC_REGDEF_API_OP_STOP                              0x01UL
#define PBE_GENERIC_REGDEF_API_OP_EOPSTOP                           0x02UL
#define PBE_GENERIC_REGDEF_API_OP_TX                                0x10UL
#define PBE_GENERIC_REGDEF_API_OP_RX                                0x11UL
#define PBE_GENERIC_REGDEF_API_OP_FS                                0x12UL
#define PBE_GENERIC_REGDEF_API_OP_STOPFS                            0x13UL

#endif