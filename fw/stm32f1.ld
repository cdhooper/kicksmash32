MEMORY {
    rom (rx) : ORIGIN = 0x08000000, LENGTH = 256K
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

INCLUDE cortex-m-generic.ld
