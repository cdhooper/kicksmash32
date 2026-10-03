#ifndef _CPU_FAULT_H
#define _CPU_FAULT_H
#endif /* _CPU_FAULT_H */

#ifdef AMIGA
#ifdef _DCC
void CPU_FAULT_ALINE(void);
void CPU_FAULT_ADDR(void);
void CPU_FAULT_CHK(void);
void CPU_FAULT_DIV0(void);
void CPU_FAULT_FLINE(void);
void CPU_FAULT_FMT(void);
void CPU_FAULT_FDIV(void);
void CPU_FAULT_FPCP(void);
void CPU_FAULT_FPUC(void);
void CPU_FAULT_ILL_INST(void);
void CPU_FAULT_PRIV(void);
void CPU_FAULT_TRAP(void);
void CPU_FAULT_TRAPV(void);
#else /* !_DCC */
#define CPU_FAULT_ALINE()    __asm(".word 0xa000");
#define CPU_FAULT_ADDR()     __asm("lea.l 0x1(pc),a0\n\t" \
                                   "jmp (a0)"::: "a0");
#define CPU_FAULT_CHK()      __asm("move.l #-1, d0\n\t" \
                                   "chk.l  #10, d0"::: "d0");
#define CPU_FAULT_DIV0()     __asm("move.l #0, d0\n\t" \
                                   "divs.w #0, d0"::: "d0", "d1");
#define CPU_FAULT_FLINE()    __asm(".word 0xf000\n\t" \
                                   ".word 0x0000");
#define CPU_FAULT_FMT()      __asm("move.l #0xff000000, -(sp)\n\t" \
                                   "frestore (sp)+");
#define CPU_FAULT_FDIV()     __asm("fmove.l #0x0400, fpcr\n\t" \
                                   "fmove.l #0x0000, fpsr\n\t" \
                                   "fmove.l #42, fp0\n\t" \
                                   "fmove.l #0, fp1\n\t" \
                                   "fdiv.x fp1, fp0");  // FP0 = 42; FP1 = 0
#define CPU_FAULT_FPOE()     __asm("fmove.l #0x2000, fpcr\n\t" \
                                   "fmove.l fp0,d0\n\t" \
                                   "move.l #0x00000000, -(sp)\n\t" \
                                   "frestore (sp)+");
#define CPU_FAULT_FPUC()     __asm("move.l #0x00000000, -(sp)\n\t" \
                                   "frestore (sp)+");
#define CPU_FAULT_ILL_INST() __asm("illegal");
#define CPU_FAULT_PRIV()     __asm("move.w #0, sr\n\t" \
                                   "stop #0x2700");
#define CPU_FAULT_TRAP()     __asm("trap #7");
#define CPU_FAULT_TRAPV()    __asm("move.l #0x7fffffff, d0\n\t\n\t" \
                                   "addq.l #2, d0\n\t\n\t" \
                                   "trapv"::: "d0");
#endif
#endif
