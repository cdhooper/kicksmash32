/*
 * Audio functions.
 *
 * This source file is part of the code base for a simple Amiga ROM
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
#include <stdint.h>
#include <string.h>
#include "util.h"
#include "amiga_chipset.h"
#include "audio.h"

static const uint8_t sinewave[] = {
    0, 0, 1, 1, 2, 4, 5, 7, 10, 12, 15, 18, 21, 25, 29, 33, 37, 42, 47, 52, 57,
    62, 67, 73, 79, 85, 90, 97, 103, 109, 115, 121, 127, 134, 140, 146, 152,
    158, 165, 170, 176, 182, 188, 193, 198, 203, 208, 213, 218, 222, 226, 230,
    234, 237, 240, 243, 245, 248, 250, 251, 253, 254, 254, 255, 255, 255, 254,
    254, 253, 251, 250, 248, 245, 243, 240, 237, 234, 230, 226, 222, 218, 213,
    208, 203, 198, 193, 188, 182, 176, 170, 165, 158, 152, 146, 140, 134, 128,
    121, 115, 109, 103, 97, 90, 85, 79, 73, 67, 62, 57, 52, 47, 42, 37, 33, 29,
    25, 21, 18, 15, 12, 10, 7, 5, 4, 2, 1, 1, 0
};

static uint8_t audio_vol;

/*
 * The audio_handler() function is called in interrupt context in response
 * to an audio interrupt.
 */
void
audio_handler(void)
{
    if (audio_vol > 0) {
        audio_vol--;
        *AUD0VOL = audio_vol;  // max is 64
        *AUD1VOL = audio_vol;  // max is 64
        *AUD0PER = 300 + audio_vol;
        *AUD1PER = 350 + audio_vol;
    } else {
        *AUD0LEN = 0;
        *AUD1LEN = 0;
        *INTENA  = INTENA_AUD0 | INTENA_AUD1;  // Disable audio interrupts
    }
}

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

void
audio_init(void)
{
    uint size = MIN(128, sizeof (sinewave));
    uint8_t *adata = malloc_chipmem(size);
    uint pos;

    /* Generate sine wave audio tone */
    for (pos = 0; pos < size; pos += sizeof (sinewave))
        memcpy(&adata[pos], sinewave, sizeof (sinewave));

    audio_vol = 52;
    *AUD0LEN = size / 2;
    *AUD0VOL = audio_vol;  // max is 64
    *AUD0PER = 300; // minimum 124 (28.86 kHz)
    *AUD0LC  = (uintptr_t) adata;

    *AUD1LEN = size / 2;
    *AUD1VOL = audio_vol;  // max is 64
    *AUD1PER = 350; // minimum 124 (28.86 kHz)
    *AUD1LC  = (uintptr_t) adata;

    *DMACON   = DMACON_SET |     // Enable
                DMACON_AUD0EN |
                DMACON_AUD1EN;

    *INTREQ  = INTREQ_AUD0 | INTREQ_AUD1;  // Clear audio interrupts
    *INTENA  = INTENA_SETCLR |  // Set
               INTENA_AUD0 |    // Enable audio 0 interrupt
               INTENA_AUD1;     // Enable audio 1 interrupt
}
