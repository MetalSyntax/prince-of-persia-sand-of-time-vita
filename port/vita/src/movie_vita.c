/* reSOT Vita: los vídeos Bink del juego como MP4 (H.264 + AAC) con SceAvPlayer.
 *
 * Como Halo y THUG en la Vita (~/MD/xbox-to-vita-ports.md): Bink no se decodifica
 * en la consola. En consola (log_008/009) el Bink recompilado iba a 1-11 fps con
 * el audio entrecortado. extract_game_data.py --movies convierte VIDEO/X.int a
 * movies/X.mp4 (640x480, bandas negras incluidas).
 *
 * Enlace con el juego (vitaize.py):
 *  - kernel_path.c llama a vita_movie_note_path() con cada ruta que abre: se
 *    recuerda el último video\X.int.
 *  - target_hooks.c, en la rutina de estado de Bink (sub_0001E970), llama a
 *    vita_movie_status() antes de decodificar: 1 = sigue (sin tocar Bink),
 *    0 = terminó (EOF: el juego cierra el vídeo por su camino), -1 = no hay MP4
 *    (Bink como siempre). Saltar con A/Espacio llama a vita_movie_stop().
 *  - El hilo de GL toma los cuadros NV12 (vita_movie_frame) y los dibuja como
 *    textura VGL_YUV420P_NV12_BT601: la GPU hace la conversión de color.
 * AvPlayer va bajo s_lock: lo usan el hilo del juego, el de GL y el de audio.
 * Asignación de memoria y arranque como Shadow-Guardian-vita/source/video.cpp.
 */
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include "manifest.h"   /* DATA_DIR */

#define AV_FB_ALIGNMENT 0x40000
#define AV_TEX_BLOCKS   8

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_name[64];            /* último video\X.int abierto, sin extensión */
static char s_cur[64];             /* el que suena (para repetir los vídeos en bucle) */
static SceAvPlayerHandle s_av;
static volatile int s_playing;
static int s_module = -1;          /* -1 sin probar, 0 no disponible, 1 cargado */
static SceUID s_audio_thread = -1;
static volatile int s_audio_run;
static struct { void *base; SceUID uid; } s_tex[AV_TEX_BLOCKS];
static SceAvPlayerFrameInfo s_frame;

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

static void *av_alloc(void *arg, uint32_t alignment, uint32_t size)
{
    (void)arg;
    return memalign(alignment, size);
}

static void av_free(void *arg, void *ptr)
{
    (void)arg;
    free(ptr);
}

static void *av_alloc_texture(void *arg, uint32_t alignment, uint32_t size)
{
    SceKernelAllocMemBlockOpt opt;
    void *base = NULL;
    SceUID blk;
    int i;
    (void)arg;
    if (alignment < AV_FB_ALIGNMENT) alignment = AV_FB_ALIGNMENT;
    size = (size + alignment - 1) & ~(alignment - 1);
    memset(&opt, 0, sizeof opt);
    opt.size = sizeof opt;
    opt.attr = 0x00000004U;   /* SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT */
    opt.alignment = alignment;
    blk = sceKernelAllocMemBlock("pop-av-tex", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, &opt);
    if (blk < 0) blk = sceKernelAllocMemBlock("pop-av-tex", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, &opt);
    if (blk < 0) {
        fprintf(stderr, "[MOVIE] sin memoria para un cuadro (%u B): 0x%08X\n", (unsigned)size, (unsigned)blk);
        return NULL;
    }
    sceKernelGetMemBlockBase(blk, &base);
    sceGxmMapMemory(base, size, (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE));
    for (i = 0; i < AV_TEX_BLOCKS; i++)
        if (!s_tex[i].base) { s_tex[i].base = base; s_tex[i].uid = blk; return base; }
    sceGxmUnmapMemory(base);
    sceKernelFreeMemBlock(blk);
    return NULL;
}

static void av_free_texture(void *arg, void *ptr)
{
    int i;
    (void)arg;
    for (i = 0; i < AV_TEX_BLOCKS; i++)
        if (s_tex[i].base == ptr) {
            sceGxmUnmapMemory(ptr);
            sceKernelFreeMemBlock(s_tex[i].uid);
            s_tex[i].base = NULL;
            return;
        }
}

static int audio_thread(SceSize args, void *argp)
{
    extern void vita_thread_register(void *entry);
    int port = -1, rate = 0, len = 0;
    (void)args; (void)argp;
    vita_thread_register((void *)audio_thread);
    while (s_audio_run) {
        SceAvPlayerFrameInfo a;
        int got;
        pthread_mutex_lock(&s_lock);
        got = s_playing && sceAvPlayerGetAudioData(s_av, &a);
        pthread_mutex_unlock(&s_lock);
        if (!got) { sceKernelDelayThread(2000); continue; }
        if (port < 0 || rate != (int)a.details.audio.sampleRate) {
            int ch = a.details.audio.channelCount;
            if (port >= 0) sceAudioOutReleasePort(port);
            rate = (int)a.details.audio.sampleRate;
            len = (int)(a.details.audio.size / (ch * sizeof(int16_t)));
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, len, rate,
                                       ch >= 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO);
            if (port < 0) fprintf(stderr, "[MOVIE] sceAudioOutOpenPort: 0x%08X\n", (unsigned)port);
        }
        if (port >= 0) sceAudioOutOutput(port, a.pData);   /* bloquea al ritmo del audio */
    }
    if (port >= 0) sceAudioOutReleasePort(port);
    return 0;
}

/* Con s_lock tomado. */
static void stop_locked(void)
{
    if (!s_playing) return;
    s_playing = 0;
    sceAvPlayerStop(s_av);
    sceAvPlayerClose(s_av);
}

static int start_locked(void)
{
    char path[256];
    SceIoStat st;
    SceAvPlayerInitData init;
    if (s_module < 0) {
        s_module = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER) >= 0;
        if (!s_module) fprintf(stderr, "[MOVIE] SceAvPlayer no disponible: vídeos con Bink\n");
    }
    if (!s_module) return 0;
    snprintf(path, sizeof path, DATA_DIR "/movies/%s.mp4", s_cur);
    if (sceIoGetstat(path, &st) < 0) {
        fprintf(stderr, "[MOVIE] sin %s: Bink\n", path);
        return 0;
    }
    memset(&init, 0, sizeof init);
    init.memoryReplacement.allocate = av_alloc;
    init.memoryReplacement.deallocate = av_free;
    init.memoryReplacement.allocateTexture = av_alloc_texture;
    init.memoryReplacement.deallocateTexture = av_free_texture;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = 2;
    init.autoStart = SCE_TRUE;
    s_av = sceAvPlayerInit(&init);
    if (!s_av || ((unsigned)s_av & 0xFF000000u) == 0x80000000u) {
        fprintf(stderr, "[MOVIE] sceAvPlayerInit: 0x%08X\n", (unsigned)s_av);
        return 0;
    }
    if (sceAvPlayerAddSource(s_av, path) < 0) {
        fprintf(stderr, "[MOVIE] sceAvPlayerAddSource falló: %s\n", path);
        sceAvPlayerClose(s_av);
        return 0;
    }
    s_playing = 1;
    if (s_audio_thread < 0) {
        s_audio_run = 1;
        s_audio_thread = sceKernelCreateThread("pop-movie-audio", audio_thread, 0x10000100 - 8, 0x4000, 0, 0, NULL);
        if (s_audio_thread >= 0) sceKernelStartThread(s_audio_thread, 0, NULL);
    }
    fprintf(stderr, "[MOVIE] %s (AvPlayer, sin decodificar Bink)\n", path);
    return 1;
}

/* Hilo del juego, en cada llamada a la rutina de estado de Bink. loop: el
 * indicador de bucle del juego (vídeos de fondo): al terminar, vuelve a empezar. */
int vita_movie_status(int loop)
{
    int r;
    pthread_mutex_lock(&s_lock);
    if (!s_playing) {
        r = -1;
        if (s_name[0]) {
            memcpy(s_cur, s_name, sizeof s_cur);
            r = start_locked() ? 1 : -1;
        }
        s_name[0] = 0;   /* un intento por apertura */
    } else if (!sceAvPlayerIsActive(s_av)) {
        stop_locked();
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

/* Hilo de GL: 1 si hay un cuadro nuevo (NV12, w x h). Válido hasta la siguiente llamada. */
int vita_movie_frame(const void **nv12, unsigned *w, unsigned *h)
{
    int r = 0;
    pthread_mutex_lock(&s_lock);
    if (s_playing && sceAvPlayerGetVideoData(s_av, &s_frame)) {
        *nv12 = s_frame.pData;
        *w = s_frame.details.video.width;
        *h = s_frame.details.video.height;
        r = 1;
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}
