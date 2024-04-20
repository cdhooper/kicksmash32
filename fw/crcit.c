/*
 * crcit
 * -----
 * Simple program to compute and verify CRC used in message communication
 * between Amiga and Kicksmash.
 *
 * cc -o crcit crcit.c crc32.c
 */
#include <stdio.h>
#include <stdint.h>
#include "crc32.h"

#define ARRAY_SIZE(x) (int)((sizeof (x) / sizeof ((x)[0])))

static const uint16_t sm_magic[] = { 0x0204, 0x1017, 0x0119, 0x0117 };

typedef unsigned int uint;

int
main(int argc, char *argv[])
{
    uint magic_pos = 0;
    uint crc = 0;
    uint crc_rx = 0;
    uint cmd_len = 0;
    uint cmd = 0;
    uint len = 0;
    uint val;
    uint16_t v;

    while (scanf("%x", &val) != EOF) {
        v = val;
        switch (magic_pos) {
            case 0:
                if (v != sm_magic[0])
                    break;
                magic_pos = 1;
                break;
            case 1 ... (ARRAY_SIZE(sm_magic) - 1):
                /* Magic phase */
                if (v != sm_magic[magic_pos]) {
                    magic_pos = 0;  // No match
                    break;
                }
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic):
                /* Length phase */
                len = cmd_len = v;
                crc = crc32r(0, &len, sizeof (uint16_t));
                printf("crc at len=%08x\n", crc);
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 1:
                /* Command phase */
                cmd = v;
                crc = crc32r(crc, &cmd, sizeof (uint16_t));
                printf("crc at cmd=%08x\n", crc);
                if (len == 0)
                    magic_pos++;  // Skip following Data Phase
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 2:
                /* Data phase */
                len--;
                if (len != 0) {
                    crc = crc32r(crc, (void *) &v, 2);
                    len--;
                } else {
                    /* Special case -- odd byte at end */
                    printf("odd last byte=%02x\n", *((uint8_t *) &v));
                    crc = crc32(crc, (uint8_t *) &v, 1);
                }
                if (len == 0)
                    magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 3:
                /* Top half of CRC */
                crc_rx = v << 16;
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 4:
                /* Bottom half of CRC */
                crc_rx |= v;
                if (crc_rx != crc) {
                    printf("cmd=%x l=%04x CRC %08x != calc %08x\n",
                           cmd, cmd_len, crc_rx, crc);
                } else {
                    printf("CRC %08x good\n", crc_rx);
                }
                magic_pos = 0;
                break;
        }
    }
}
