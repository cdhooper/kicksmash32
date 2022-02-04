/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Generic physical memory access code.
 */

#ifndef _MEM_ACCESS_H
#define _MEM_ACCESS_H

rc_t mem_read(uint64_t addr, uint width, void *bufp);
rc_t mem_write(uint64_t addr, uint width, void *bufp);

extern uint8_t mem_fault_ok;
extern uint    mem_fault_count;

#endif /* _MEM_ACCESS_H */
