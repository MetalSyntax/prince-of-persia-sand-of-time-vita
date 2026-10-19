/**
 * reSOT Vita - pantalla: lo que video/fb_present.c hace en Windows/Android
 * (mostrar el framebuffer que el juego dejó en memoria guest), con sceDisplay.
 *
 * V0: el ejecutor NV2A rasteriza por software en la memoria guest
 * (RECOMP_HW=0); en cada flip se copia el frame 640x480 y el hilo principal
 * lo escala a 725x544 (4:3 centrado) en el framebuffer de la Vita. Sin vitaGL:
 * el backend GPU propio es V1.1.
 */
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>

#include "vvm.h"

extern ptrdiff_t g_xbox_mem_offset;

#define SCR_W   960
#define SCR_H   544
#define SRC_W   640
#define SRC_H   480
#define DST_W   725            /* 544 * 4/3 */
#define DST_X   ((SCR_W - DST_W) / 2)
#define FB_SIZE 0x200000       /* 960*544*4 redondeado a 256 KB */

static uint32_t *s_screen[2];
static int       s_back;
static uint32_t  s_frame[SRC_W * SRC_H];   /* último frame, ya en ABGR */
static volatile int s_frame_serial, s_shown_serial = -1;
static pthread_mutex_t s_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t  s_fb_va, s_fb_pitch;
static uint16_t  s_xmap[DST_W];

static void flip(void)
{
    SceDisplayFrameBuf fb = { sizeof fb, s_screen[s_back], SCR_W,
                              SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, SCR_W, SCR_H };
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    s_back ^= 1;
}

void vita_fb_init(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        SceUID id = sceKernelAllocMemBlock("pop-fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_SIZE, NULL);
        sceKernelGetMemBlockBase(id, (void **)&s_screen[i]);
        memset(s_screen[i], 0, FB_SIZE);
    }
    for (i = 0; i < DST_W; i++) s_xmap[i] = (uint16_t)(i * SRC_W / DST_W);
    flip();
}

/* Pantalla de un color (error de arranque: rojo; el detalle va al log). */
void vita_fb_message(uint32_t rgb)
{
    uint32_t abgr = 0xFF000000u | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
    int i, k;
    for (k = 0; k < 2; k++) {
        for (i = 0; i < SCR_W * SCR_H; i++) s_screen[s_back][i] = abgr;
        flip();
    }
    s_shown_serial = s_frame_serial;  /* que pump no lo pise con un frame viejo */
}

/* El frame del juego: X8R8G8B8 (pitch >= 2560) o R5G6B5. */
static void grab(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    int y, x, bpp = pitch >= SRC_W * 4 ? 4 : 2;
    if (!fb_va || !pitch) return;
    pthread_mutex_lock(&s_frame_lock);
    for (y = 0; y < SRC_H; y++) {
        uint32_t *d = &s_frame[y * SRC_W];
        /* fila a fila: el framebuffer está en un solo almacén, pero así no se asume */
        src = (const uint8_t *)vvm_ptr((uint32_t)(fb_va + (uint32_t)y * pitch) + (uint32_t)g_xbox_mem_offset);
        if (bpp == 4) {
            const uint32_t *s = (const uint32_t *)src;
            for (x = 0; x < SRC_W; x++) {
                uint32_t p = s[x];
                d[x] = 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
            }
        } else {
            const uint16_t *s = (const uint16_t *)src;
            for (x = 0; x < SRC_W; x++) {
                uint32_t p = s[x];
                uint32_t r = (p >> 11) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
                d[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
            }
        }
    }
    s_frame_serial++;
    pthread_mutex_unlock(&s_frame_lock);
}

/* Hilo principal: escala el último frame si hay uno nuevo. */
void vita_fb_pump(void)
{
    int y, x;
    if (s_shown_serial == s_frame_serial) return;
    pthread_mutex_lock(&s_frame_lock);
    for (y = 0; y < SCR_H; y++) {
        const uint32_t *s = &s_frame[(y * SRC_H / SCR_H) * SRC_W];
        uint32_t *d = &s_screen[s_back][y * SCR_W + DST_X];
        for (x = 0; x < DST_W; x++) d[x] = s[s_xmap[x]];
    }
    s_shown_serial = s_frame_serial;
    pthread_mutex_unlock(&s_frame_lock);
    flip();
}

/* ── la interfaz de video/fb_present.c ─────────────────────── */

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { s_fb_va = fb_va; s_fb_pitch = pitch; }
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    if (!fb_va) { fb_va = s_fb_va; pitch = s_fb_pitch; }
    grab(fb_va, pitch);
}
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowSetTitle(const uint16_t *n, int m) { (void)n; (void)m; }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
int  xbox_FramebufferKeyDown(int vk) { (void)vk; return 0; }
void xbox_FramebufferTakeMouse(int *dx, int *dy) { *dx = 0; *dy = 0; }
void xbox_SetVSync(int enabled) { (void)enabled; }
int  xbox_FramebufferVSync(void) { return 1; }
void xbox_SetPostProcessing(int enabled) { (void)enabled; }
int  xbox_FramebufferPostProcessing(void) { return 0; }
void xbox_SetFrameLimit(int fps) { (void)fps; }
void xbox_SetRenderScalePercent(int percent) { (void)percent; }
void xbox_FramebufferLimitFrame(void) {}
int  xbox_FramebufferDeferPresent(void) { return 0; }
void xbox_FramebufferSetOptionsCallbacks(int (*panel)(const uint32_t **, uint32_t *, uint32_t *),
                                         int (*event)(void *, unsigned, uintptr_t, intptr_t))
{ (void)panel; (void)event; }
void xbox_FramebufferSetWowProvider(void (*provider)(char *, size_t)) { (void)provider; }
int  xbox_FramebufferDebugPanelGet(const uint32_t **pixels, uint32_t *width, uint32_t *height)
{ (void)pixels; (void)width; (void)height; return 0; }
void *xbox_FramebufferWindowHandle(void) { return NULL; }
void xbox_FramebufferWindowHwMode(int on) { (void)on; }
void xbox_FramebufferHostKey(int vk, int down) { (void)vk; (void)down; }
void xbox_FramebufferHostMouse(int dx, int dy) { (void)dx; (void)dy; }
void xbox_FramebufferToggleDebugPanel(void) {}
void resot_host_draw_text(uint32_t *bits, int w, int h, int x, int y, const char *text, uint32_t argb)
{ (void)bits; (void)w; (void)h; (void)x; (void)y; (void)text; (void)argb; }
