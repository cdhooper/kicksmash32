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

/*
 * Startup "ball bounce" sound
 * ---------------------------
 * The sound is a short tone which fades out linearly while its pitch rises
 * slightly. It used to be produced by looping one cycle of the waveform
 * below and taking an audio interrupt at the end of every cycle (about 50
 * interrupts) to step the Paula volume down and the period toward its final
 * value.
 *
 * It is now rendered once into chip memory with the volume fade and the
 * pitch change baked into the sample data, and then played as a one-shot
 * by audio DMA at a fixed period and volume. No audio interrupt is enabled
 * or taken:
 *   - Paula only reads AUDxLC / AUDxLEN when a channel starts and again
 *     each time it reaches the end of the waveform. Once the channel has
 *     started, these registers are pointed at a two-sample silent loop.
 *     Paula switches to that loop by itself when the one-shot ends.
 *   - Paula sets the channel's INTREQ bit when it has taken the first
 *     AUDxLC / AUDxLEN values. That bit is polled in INTREQR to know when
 *     the registers may be rewritten. INTENA for audio stays off, so the
 *     request never reaches the CPU.
 *
 * Both channels play the same data. Channel 1 uses a longer period, which
 * gives the same low/high tone pair (left/right) as before.
 */
#define BOUNCE_SHIFT    12                   // log2 of sample count
#define BOUNCE_SAMPLES  (1 << BOUNCE_SHIFT)  // 4096 samples: ~0.35 sec
#define BOUNCE_VOL      52                   // Paula volume at start (max 64)
#define BOUNCE_PERIOD0  300                  // Channel 0 period (~11.8 kHz)
#define BOUNCE_PERIOD1  350                  // Channel 1 period (~10.1 kHz)
#define BOUNCE_SWEEP    52                   // Period starts this much longer
#define BOUNCE_STEP_INT 32                   // Samples between pitch updates
#define PHASE_SHIFT     12                   // Waveform position fraction bits

/*
 * One cycle of the waveform. Paula samples are signed, so the upper half of
 * this unsigned raised cosine wraps negative. The two resulting jumps are
 * what give the tone its buzz, so the table is kept exactly as it was.
 */
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

/*
 * audio_handler() is called in interrupt context in response to an audio
 *                 interrupt. Audio interrupts are no longer enabled, so
 *                 this is not expected to run. If it does, just make sure
 *                 that it does not happen again.
 */
void
audio_handler(void)
{
    *INTENA = INTENA_AUD0 | INTENA_AUD1 | INTENA_AUD2 | INTENA_AUD3;
}

/*
 * audio_render_bounce() fills the buffer with the complete sound.
 *
 * At sample n of N, the old interrupt handler would have had
 *     volume = BOUNCE_VOL * (N - n) / N
 *     period = BOUNCE_PERIOD0 + BOUNCE_SWEEP * (N - n) / N
 * Volume is applied here by scaling the sample, and period by how far the
 * position in the waveform advances for each output sample (output is at
 * the fixed BOUNCE_PERIOD0 rate, so a longer period is a smaller advance).
 */
static void
audio_render_bounce(int8_t *buf)
{
    uint32_t phase = 0;  // Position in waveform, PHASE_SHIFT fraction bits
    uint32_t step  = 0;  // Phase advance per output sample
    uint     n;

    for (n = 0; n < BOUNCE_SAMPLES; n++) {
        uint remain = BOUNCE_SAMPLES - n;      // BOUNCE_SAMPLES down to 1
        int  sample;

        if ((n % BOUNCE_STEP_INT) == 0) {
            /* period in 1/256 units; step = PERIOD0 / period */
            uint32_t period = (BOUNCE_PERIOD0 << 8) +
                              ((BOUNCE_SWEEP * remain) >>
                               (BOUNCE_SHIFT - 8));
            step = ((uint32_t) BOUNCE_PERIOD0 << (8 + PHASE_SHIFT)) / period;
        }

        sample = (int8_t) sinewave[(phase >> PHASE_SHIFT) % sizeof (sinewave)];
        sample = (sample * (int) remain) / BOUNCE_SAMPLES;  // Linear fade
        buf[n] = sample;
        phase += step;
    }

    /* Silent loop which Paula plays after the one-shot */
    buf[BOUNCE_SAMPLES]     = 0;
    buf[BOUNCE_SAMPLES + 1] = 0;
}

void
audio_init(void)
{
    const uint16_t aud_irqs = INTREQ_AUD0 | INTREQ_AUD1;
    int8_t *adata = malloc_chipmem(BOUNCE_SAMPLES + 2);
    uint    timeout;

    /* Audio never interrupts; stop channels in case they were running */
    *INTENA = INTENA_AUD0 | INTENA_AUD1 | INTENA_AUD2 | INTENA_AUD3;
    *DMACON = DMACON_AUD0EN | DMACON_AUD1EN;
    if (adata == NULL)
        return;

    audio_render_bounce(adata);

    *AUD0LC  = (uintptr_t) adata;
    *AUD0LEN = BOUNCE_SAMPLES / 2;  // Length is in words
    *AUD0PER = BOUNCE_PERIOD0;      // minimum 124 (28.86 kHz)
    *AUD0VOL = BOUNCE_VOL;          // max is 64

    *AUD1LC  = (uintptr_t) adata;
    *AUD1LEN = BOUNCE_SAMPLES / 2;
    *AUD1PER = BOUNCE_PERIOD1;
    *AUD1VOL = BOUNCE_VOL;

    *INTREQ  = aud_irqs;            // Clear audio requests
    *DMACON  = DMACON_SET |         // Start both channels
               DMACON_AUD0EN |
               DMACON_AUD1EN;

    /*
     * Wait for both channels to take the location and length. This is
     * normally within two raster lines (128 usec). The timeout is several
     * milliseconds at the least, on any CPU.
     */
    for (timeout = 20000; timeout != 0; timeout--)
        if ((*INTREQR & aud_irqs) == aud_irqs)
            break;

    /* Next waveform for both channels is the silent loop */
    *AUD0LC  = (uintptr_t) adata + BOUNCE_SAMPLES;
    *AUD0LEN = 1;
    *AUD1LC  = (uintptr_t) adata + BOUNCE_SAMPLES;
    *AUD1LEN = 1;
    *INTREQ  = aud_irqs;
}
