/*
 * MED commands specific to Amiga CPU.
 *
 * This header file is part of the code base for a simple Amiga ROM
 * replacement sufficient to allow programs using some parts of GadTools
 * to function.
 *
 * Copyright 2025 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
#ifndef _MED_CPU_H
#define _MED_CPU_H

rc_t cmd_cpu(int argc, char * const *argv);
rc_t cmd_dis(int argc, char * const *argv);

extern const char cmd_cpu_help[];
extern const char cmd_dis_help[];

#endif /* _MED_CPU_H */
