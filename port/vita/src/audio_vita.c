/**
 * reSOT Vita - salida de audio de la APU emulada: la interfaz xa2_* de
 * src/apu/apu_xaudio2.h sobre SceAudio, con la política de cola de
 * port/android/src/audio_android.c: tramas de 256 muestras estéreo de 16 bits
 * a 48 kHz. Cola más honda que en Android: en consola la APU llega hasta 94 ms
 * tarde (log_008: 30-60 % de tramas sin datos, sonido entrecortado), así que se
 * ceba con 12 de silencio (64 ms) al vaciarse y se descarta a partir de 32 (170 ms).
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include "apu_xaudio2.h"

#define XA2_SAMPLE_RATE 48000
#define XA2_BUF_SAMPLES 256
#define XA2_QUEUE_PRIME 12
#define XA2_QUEUE_MAX   32
#define RING            48   /* > XA2_QUEUE_MAX */

static int16_t s_ring[RING][XA2_BUF_SAMPLES * 2];
static volatile int s_head, s_tail;   /* productor: APU; consumidor: s_thread */
static int s_port = -1, s_frames_written, s_dropped, s_starved;
static SceUID s_thread = -1;
static volatile int s_run;

static int queued(void) { return (s_head - s_tail + RING) % RING; }

extern void vita_thread_register(void *entry);

static int out_thread(SceSize args, void *argp)
{
    static const int16_t silence[XA2_BUF_SAMPLES * 2];
    (void)args; (void)argp;
#ifdef __vita__
    vita_thread_register((void *)out_thread);
#endif
    while (s_run) {
        if (queued()) {
            sceAudioOutOutput(s_port, s_ring[s_tail]);   /* bloquea hasta consumir la trama */
            s_tail = (s_tail + 1) % RING;
        } else {
            sceAudioOutOutput(s_port, silence);
        }
    }
    return 0;
}

int xa2_init(void)
{
    if (s_port >= 0) return 1;
    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, XA2_BUF_SAMPLES, XA2_SAMPLE_RATE,
                                 SCE_AUDIO_OUT_MODE_STEREO);
    if (s_port < 0) {
        fprintf(stderr, "[XA2] sceAudioOutOpenPort falló: 0x%08X\n", s_port);
        return 0;
    }
    s_run = 1;
    s_thread = sceKernelCreateThread("pop-audio", out_thread, 0x10000100, 0x4000, 0, 0, NULL);
    sceKernelStartThread(s_thread, 0, NULL);
    fprintf(stderr, "[XA2] SceAudio %d Hz estéreo, tramas de %d muestras\n",
            XA2_SAMPLE_RATE, XA2_BUF_SAMPLES);
    return 1;
}

void xa2_shutdown(void)
{
    if (s_port < 0) return;
    s_run = 0;
    sceKernelWaitThreadEnd(s_thread, NULL, NULL);
    sceKernelDeleteThread(s_thread);
    sceAudioOutReleasePort(s_port);
    s_port = -1;
    fprintf(stderr, "[XA2] cerrado (%d tramas)\n", s_frames_written);
}

int xa2_is_active(void) { return s_port >= 0; }

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int k;
    if (s_port < 0) return 0;
    if (num_samples > XA2_BUF_SAMPLES) num_samples = XA2_BUF_SAMPLES;
    if (queued() >= XA2_QUEUE_MAX) { s_dropped++; return 0; }
    if (!queued()) {
        if (s_frames_written) s_starved++;
        for (k = 0; k < XA2_QUEUE_PRIME; k++) {
            memset(s_ring[s_head], 0, sizeof s_ring[0]);
            s_head = (s_head + 1) % RING;
        }
    }
    memset(s_ring[s_head], 0, sizeof s_ring[0]);
    memcpy(s_ring[s_head], samples, (size_t)num_samples * 4);
    s_head = (s_head + 1) % RING;
    s_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void) { return XA2_BUF_SAMPLES; }

void xa2_get_stats(int *q, int *dropped, int *starved)
{
    if (q) *q = queued();
    if (dropped) *dropped = s_dropped;
    if (starved) *starved = s_starved;
}
