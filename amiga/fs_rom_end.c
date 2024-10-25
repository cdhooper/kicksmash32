#include <stdio.h>

#if 0
#define STR2(x) #x
#define STR(x) STR2(x)

#ifdef __APPLE__
#define USTR(x) "_" STR(x)
#else
#define USTR(x) STR(x)
#endif

#ifdef _WIN32
#define INCBIN_SECTION ".rdata, \"dr\""
#elif defined __APPLE__
#define INCBIN_SECTION "__TEXT,__const"
#else
#define INCBIN_SECTION ".rodata"
#endif

// this aligns start address to 16 and terminates byte array with explict 0
// which is not really needed, feel free to change it to whatever you want/need
#define INCBIN(prefix, name, file) \
    __asm__(".section " INCBIN_SECTION "\n" \
            ".global " USTR(prefix) "_" STR(name) "_start\n" \
            ".balign 16\n" \
            USTR(prefix) "_" STR(name) "_start:\n" \
            ".incbin \"" file "\"\n" \
            \
            ".global " STR(prefix) "_" STR(name) "_end\n" \
            ".balign 1\n" \
            USTR(prefix) "_" STR(name) "_end:\n" \
            ".byte 0\n" \
    ); \
    extern __attribute__((aligned(16))) const char prefix ## _ ## name ## _start[]; \
    extern                              const char prefix ## _ ## name ## _end[];

__asm("xdef _smashfs_start");
__asm("_smashfs_start:");

INCBIN(incbin, smashfs, "smashfs.strip");
#endif

__asm("xdef _rom_end");
__asm("_rom_end:");
