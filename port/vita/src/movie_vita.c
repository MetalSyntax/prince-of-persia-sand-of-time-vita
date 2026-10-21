/* reSOT Vita: los vídeos Bink originales (VIDEO/*.int) decodificados con FFmpeg.
 *
 * En consola el Bink del juego (x86 recompilado, cada acceso por vvm_ptr) iba a
 * 1-11 fps con el audio entrecortado. El decodificador Bink de FFmpeg en C para
 * ARM tarda ~0,3 ms por cuadro en un M4 (est. 11-18 ms en la Vita; el vídeo es
 * de 29,97 fps). Se usan los mismos archivos del disco, sin convertir nada
 * (port/vita/tools/build_ffmpeg_bink.sh compila el FFmpeg mínimo).
 *
 * Enlace con el juego (vitaize.py):
 *  - kernel_path.c llama a vita_movie_note_path() con cada ruta que abre: se
 *    recuerda el último video\X.int.
 *  - target_hooks.c, en la rutina de estado de Bink (sub_0001E970), llama a
 *    vita_movie_status() antes de decodificar: 1 = sigue (el juego no toca
 *    Bink), 0 = terminó (EOF: el juego cierra el vídeo por su camino), -1 = no
 *    se pudo empezar (Bink del juego). Saltar con A/Espacio: vita_movie_stop().
 *  - El hilo de GL toma el cuadro que toca (vita_movie_frame) y lo dibuja como
 *    textura VGL_YUV420P_BT601: la GPU hace la conversión de color.
 * Hilos: el decodificador va por delante (VQ cuadros, hasta 1 s de audio); el
 * de audio saca el anillo por un puerto VOICE; los cuadros se muestran por su
 * pts con el reloj de pared desde el primero.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "manifest.h"   /* DATA_DIR */

#define VQ        4                 /* cuadros decodificados por delante */
#define AQ        (48000 * 2)       /* anillo de audio: 1 s estéreo s16 */
#define AUDIO_LEN 1024              /* muestras por salida de SceAudio */

extern void vita_thread_register(void *entry);

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_name[64];            /* último video\X.int abierto, sin extensión */
static char s_cur[64];             /* el que suena (para repetir los vídeos en bucle) */
static volatile int s_playing;     /* 1 mientras suena; 0 al acabar o al pararlo */
static volatile int s_stop;        /* pide al decodificador que termine */
static volatile unsigned s_gen;    /* sube con cada vídeo: el hilo de GL reinicia su cuenta */
static SceUID s_dec_thread = -1, s_audio_thread = -1;

static struct { uint8_t *buf; unsigned w, h; int64_t pts_us; volatile int full; } s_vq[VQ];
static unsigned s_vq_w;            /* siguiente hueco a llenar (decodificador) */
static int16_t s_aq[AQ * 2];
static volatile unsigned s_aq_w, s_aq_r;   /* en muestras s16 */
static volatile int s_aq_rate = 48000;

void vita_movie_note_path(const char *xbox_path)
{
    const char *v = NULL, *p, *dot;
    size_t n;
    for (p = xbox_path; *p; p++)
        if ((p[0] == '\\' || p[0] == '/') && !strncasecmp(p + 1, "video", 5) && (p[6] == '\\' || p[6] == '/'))
            v = p + 7;
    if (!v || !(dot = strrchr(v, '.')) || strcasecmp(dot, ".int") || (n = (size_t)(dot - v)) >= sizeof s_name)
        return;
    pthread_mutex_lock(&s_lock);
    memcpy(s_name, v, n);
    s_name[n] = 0;
    pthread_mutex_unlock(&s_lock);
}

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static void push_audio(const AVFrame *f)
{
    int ch = f->ch_layout.nb_channels, i;
    int planar = f->format == AV_SAMPLE_FMT_FLTP;
    if (!planar && f->format != AV_SAMPLE_FMT_FLT) return;   /* binkaudio da float */
    for (i = 0; i < f->nb_samples; i++) {
        float l = planar ? ((const float *)f->extended_data[0])[i] : ((const float *)f->data[0])[i * ch];
        float r = ch < 2 ? l : planar ? ((const float *)f->extended_data[1])[i] : ((const float *)f->data[0])[i * ch + 1];
        while (s_aq_w - s_aq_r + 2 > AQ * 2 && !s_stop) sceKernelDelayThread(2000);   /* anillo lleno */
        l = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
        r = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
        s_aq[s_aq_w % (AQ * 2)] = (int16_t)(l * 32767.0f);
        s_aq[(s_aq_w + 1) % (AQ * 2)] = (int16_t)(r * 32767.0f);
        s_aq_w += 2;
    }
}

static void push_video(const AVFrame *f, int64_t pts_us)
{
    unsigned w = (unsigned)f->width, h = (unsigned)f->height, y, slot = s_vq_w % VQ;
    uint8_t *d;
    while (s_vq[slot].full && !s_stop) sceKernelDelayThread(2000);   /* cola llena: va por delante */
    if (s_stop) return;
    if (!s_vq[slot].buf || s_vq[slot].w != w || s_vq[slot].h != h) {
        free(s_vq[slot].buf);
        if (!(s_vq[slot].buf = (uint8_t *)malloc(w * h * 3 / 2))) return;
        s_vq[slot].w = w; s_vq[slot].h = h;
    }
    d = s_vq[slot].buf;   /* Y, U, V contiguos: lo que espera VGL_YUV420P_BT601 */
    for (y = 0; y < h; y++) memcpy(d + y * w, f->data[0] + y * f->linesize[0], w);
    d += w * h;
    for (y = 0; y < h / 2; y++) memcpy(d + y * (w / 2), f->data[1] + y * f->linesize[1], w / 2);
    d += (w / 2) * (h / 2);
    for (y = 0; y < h / 2; y++) memcpy(d + y * (w / 2), f->data[2] + y * f->linesize[2], w / 2);
    s_vq[slot].pts_us = pts_us;
    s_vq[slot].full = 1;
    s_vq_w++;
}

static int decode_thread(SceSize args, void *argp)
{
    char path[256];
    AVFormatContext *fmt = NULL;
    AVCodecContext *vc = NULL, *ac = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    int vs = -1, as = -1, i, frames = 0;
    uint64_t t_dec = 0;
    (void)args; (void)argp;
    vita_thread_register((void *)decode_thread);
    snprintf(path, sizeof path, "file:" DATA_DIR "/VIDEO/%s.int", s_cur);
    if (!pkt || !f || avformat_open_input(&fmt, path, NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "[MOVIE] no se pudo abrir %s\n", path);
        goto out;
    }
    for (i = 0; i < (int)fmt->nb_streams; i++) {
        AVCodecParameters *par = fmt->streams[i]->codecpar;
        const AVCodec *dec = avcodec_find_decoder(par->codec_id);
        AVCodecContext **ctx = par->codec_type == AVMEDIA_TYPE_VIDEO && vs < 0 ? &vc
                             : par->codec_type == AVMEDIA_TYPE_AUDIO && as < 0 ? &ac : NULL;
        if (!ctx || !dec || !(*ctx = avcodec_alloc_context3(dec))) continue;
        if (avcodec_parameters_to_context(*ctx, par) < 0 || avcodec_open2(*ctx, dec, NULL) < 0) {
            avcodec_free_context(ctx);
            continue;
        }
        if (ctx == &vc) vs = i; else as = i;   /* la primera pista de audio (cine_820 trae 20) */
    }
    if (vs < 0) { fprintf(stderr, "[MOVIE] %s sin vídeo Bink decodificable\n", path); goto out; }
    if (ac) s_aq_rate = ac->sample_rate;
    fprintf(stderr, "[MOVIE] %s: %dx%d, audio %s (FFmpeg, sin el Bink del juego)\n", path,
            vc->width, vc->height, ac ? "sí" : "no");
    while (!s_stop && av_read_frame(fmt, pkt) >= 0) {
        AVCodecContext *c = pkt->stream_index == vs ? vc : pkt->stream_index == as ? ac : NULL;
        uint64_t t = now_us();
        if (c && avcodec_send_packet(c, pkt) >= 0) {
            while (!s_stop && avcodec_receive_frame(c, f) >= 0) {
                if (c == vc) {
                    int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : frames;
                    t_dec += now_us() - t;
                    push_video(f, av_rescale_q(pts, fmt->streams[vs]->time_base, (AVRational){1, 1000000}));
                    if (++frames % 300 == 0)
                        fprintf(stderr, "[MOVIE] %d cuadros, decodificación media %.1f ms\n",
                                frames, (double)t_dec / 1000.0 / frames);
                    t = now_us();
                } else {
                    push_audio(f);
                }
            }
        }
        av_packet_unref(pkt);
    }
    while (!s_stop) {   /* que se vean los cuadros que quedan */
        int left = 0;
        for (i = 0; i < VQ; i++) left |= s_vq[i].full;
        if (!left) break;
        sceKernelDelayThread(5000);
    }
out:
    avcodec_free_context(&vc);
    avcodec_free_context(&ac);
    avformat_close_input(&fmt);
    av_frame_free(&f);
    av_packet_free(&pkt);
    s_playing = 0;
    return sceKernelExitThread(0);
}

static int audio_thread(SceSize args, void *argp)
{
    static int16_t out[AUDIO_LEN * 2];
    int port = -1, rate = 0;
    (void)args; (void)argp;
    vita_thread_register((void *)audio_thread);
    for (;;) {
        unsigned i, r = s_aq_r, w = s_aq_w;
        if (!s_playing) { sceKernelDelayThread(10000); continue; }
        if (port < 0 || rate != s_aq_rate) {
            if (port >= 0) sceAudioOutReleasePort(port);
            rate = s_aq_rate;
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, AUDIO_LEN, rate, SCE_AUDIO_OUT_MODE_STEREO);
            if (port < 0) {
                fprintf(stderr, "[MOVIE] sceAudioOutOpenPort: 0x%08X\n", (unsigned)port);
                sceKernelDelayThread(100000);
                continue;
            }
        }
        for (i = 0; i < AUDIO_LEN * 2; i++) out[i] = r + i < w ? s_aq[(r + i) % (AQ * 2)] : 0;
        s_aq_r = r + AUDIO_LEN * 2 < w ? r + AUDIO_LEN * 2 : w;
        sceAudioOutOutput(port, out);   /* bloquea al ritmo del audio */
    }
    return 0;
}

/* Con s_lock tomado: espera al decodificador (ya terminado o parado) y lo borra. */
static void join_locked(void)
{
    if (s_dec_thread < 0) return;
    sceKernelWaitThreadEnd(s_dec_thread, NULL, NULL);
    sceKernelDeleteThread(s_dec_thread);
    s_dec_thread = -1;
}

static void stop_locked(void)
{
    int i;
    s_stop = 1;
    join_locked();
    s_playing = 0;
    for (i = 0; i < VQ; i++) s_vq[i].full = 0;
}

static int start_locked(void)
{
    int i;
    s_stop = 0;
    s_vq_w = 0;
    s_aq_r = s_aq_w = 0;
    for (i = 0; i < VQ; i++) s_vq[i].full = 0;
    s_gen++;
    s_playing = 1;
    s_dec_thread = sceKernelCreateThread("pop-movie-dec", decode_thread, 0x10000100 - 8, 256 * 1024, 0, 0, NULL);
    if (s_dec_thread < 0 || sceKernelStartThread(s_dec_thread, 0, NULL) < 0) {
        if (s_dec_thread >= 0) sceKernelDeleteThread(s_dec_thread);
        s_dec_thread = -1;
        s_playing = 0;
        return 0;
    }
    if (s_audio_thread < 0) {
        s_audio_thread = sceKernelCreateThread("pop-movie-audio", audio_thread, 0x10000100 - 16, 0x4000, 0, 0, NULL);
        if (s_audio_thread >= 0) sceKernelStartThread(s_audio_thread, 0, NULL);
    }
    return 1;
}

/* Hilo del juego, en cada llamada a la rutina de estado de Bink. loop: el
 * indicador de bucle del juego (vídeos de fondo): al terminar, vuelve a empezar. */
int vita_movie_status(int loop)
{
    int r;
    pthread_mutex_lock(&s_lock);
    if (s_dec_thread < 0) {
        r = -1;
        if (s_name[0]) {
            memcpy(s_cur, s_name, sizeof s_cur);
            r = start_locked() ? 1 : -1;
        }
        s_name[0] = 0;   /* un intento por apertura */
    } else if (!s_playing) {
        join_locked();
        r = loop && start_locked() ? 1 : 0;
        if (!r) fprintf(stderr, "[MOVIE] terminado\n");
    } else {
        r = 1;
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}

void vita_movie_stop(void)
{
    pthread_mutex_lock(&s_lock);
    stop_locked();
    pthread_mutex_unlock(&s_lock);
}

int vita_movie_active(void) { return s_playing; }

/* Hilo de GL: 1 si toca mostrar un cuadro nuevo (YUV420P contiguo, w x h). Los
 * que ya pasaron de su hora se descartan; el puntero vale hasta la siguiente
 * llamada (su hueco no se libera antes). */
int vita_movie_frame(const void **yuv, unsigned *w, unsigned *h)
{
    static unsigned gen, shown;
    static int held = -1;
    static uint64_t t0;
    if (gen != s_gen) { gen = s_gen; shown = 0; held = -1; t0 = 0; }
    if (held >= 0) { s_vq[held].full = 0; held = -1; }
    while (s_playing && s_vq[shown % VQ].full) {
        unsigned slot = shown % VQ, next = (shown + 1) % VQ;
        int64_t t = (int64_t)now_us();
        if (!t0) t0 = (uint64_t)(t - s_vq[slot].pts_us);
        t -= (int64_t)t0;
        if (t < s_vq[slot].pts_us) return 0;   /* aún no es su hora */
        shown++;
        if (s_vq[next].full && t >= s_vq[next].pts_us) { s_vq[slot].full = 0; continue; }   /* llegó tarde */
        *yuv = s_vq[slot].buf; *w = s_vq[slot].w; *h = s_vq[slot].h;
        held = (int)slot;
        return 1;
    }
    return 0;
}
