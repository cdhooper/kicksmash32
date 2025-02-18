/*
 * Amiga exec standard definitions
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
#ifndef _EXEC_TYPES_H
#define _EXEC_TYPES_H

/* Exec types */
typedef void *   APTR;
typedef int8_t   BYTE;
typedef uint8_t  UBYTE;
typedef int16_t  WORD;
typedef uint16_t UWORD;
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef int16_t  SHORT;
typedef uint16_t USHORT;
typedef unsigned char *STRPTR;

typedef struct Node
{
    struct  Node *ln_Succ;  // Pointer to next (successor)
    struct  Node *ln_Pred;  // Pointer to previous (predecessor)
    uint8_t       ln_Type;
    int8_t        ln_Pri;   // Priority, for sorting
    char         *ln_Name;  // ID string, null terminated
} Node;

typedef struct MinNode {
    struct MinNode *mln_Succ;
    struct MinNode *mln_Pred;
} MinNode;

typedef struct MinList {
   struct MinNode *mlh_Head;
   struct MinNode *mlh_Tail;
   struct MinNode *mlh_TailPred;
} MinList;

typedef struct List {
   struct  Node *lh_Head;
   struct  Node *lh_Tail;
   struct  Node *lh_TailPred;
   uint8_t lh_Type;
   uint8_t l_pad;
} List;

#define FALSE 0
#define TRUE  1

#endif /* _EXEC_TYPES_H */
