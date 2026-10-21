#include <stdint.h>
#include <string.h>
#include "persist.h"

SRAM_PERSIST sram_persist_t persist;

void
persist_init(void)
{
    if ((persist.sp_magic != SP_MAGIC) ||
        (persist.sp_addr != (uintptr_t) &persist)) {
        memset(&persist, 0, sizeof (persist));
        persist.sp_magic = SP_MAGIC;
        persist.sp_addr = (uintptr_t) &persist;
    }
}
