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
#define CPU_FAULT_ADDR()     __asm("lea.l 0x1(pc),a0\n\t"); \
                             __asm("jmp (a0)"::: "a0");
#define CPU_FAULT_CHK()      __asm("move.l #-1, d0\n\t"); \
                             __asm("chk.l  #10, d0"::: "d0");
#define CPU_FAULT_DIV0()     __asm("move.l #0, d0\n\t"); \
                             __asm("divs.w #0, d0"::: "d0", "d1");
#define CPU_FAULT_FLINE()    __asm(".word 0xf000"); \
                             __asm(".word 0x0000");
#define CPU_FAULT_FMT()      __asm("move.l #0xff000000, -(sp)"); \
                             __asm("frestore (sp)+");
#define CPU_FAULT_FDIV()     __asm("fmove.l #0x0400, fpcr"); \
                             __asm("fmove.l #0x0000, fpsr"); \
                             __asm("fmove.l #42, fp0"); \
                             __asm("fmove.l #0, fp1"); \
                             __asm("fdiv.x fp1, fp0");  // FP0 = 42; FP1 = 0
#define CPU_FAULT_FPCP()     __asm("fmove.l #0x2000, fpcr"); \
                             __asm("fmove.l fp0,d0"); \
                             __asm("move.l #0x00000000, -(sp)"); \
                             __asm("frestore (sp)+");
#define CPU_FAULT_FPUC()     __asm("move.l #0x00000000, -(sp)"); \
                             __asm("frestore (sp)+");
#define CPU_FAULT_ILL_INST() __asm("illegal");
#define CPU_FAULT_PRIV()     __asm("move.w #0, sr"); \
                             __asm("stop #0x2700");
#define CPU_FAULT_TRAP()     __asm("trap #7");
#define CPU_FAULT_TRAPV()    __asm("move.l #0x7fffffff, d0\n\t"); \
                             __asm("addq.l #2, d0\n\t"); \
                             __asm("trapv"::: "d0");
#endif
#endif
