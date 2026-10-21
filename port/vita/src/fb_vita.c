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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

/* ── overlay de vídeo (PVIDEO): los Bink del juego ────────────
 * Lo de video/fb_present.c (overlay_tick/overlay_latch), que en Vita no se
 * compila: el decodificador Bink del juego deja cuadros YUY2 en la memoria
 * contigua y programa los registros PVIDEO del NV2A. Además de mostrarlos,
 * hay que contestar STOP borrando BUFFER: el D3D del juego espera en bucle a
 * que BUFFER vuelva a 0 al cerrar un vídeo. */
#define NV_REG_BASE            0xFD000000u
#define NV_PVIDEO_BUFFER       0x008700u
#define NV_PVIDEO_STOP         0x008704u
#define NV_PVIDEO_BASE(i)      (0x008900u + (i) * 4u)
#define NV_PVIDEO_OFFSET(i)    (0x008920u + (i) * 4u)
#define NV_PVIDEO_SIZE_IN(i)   (0x008928u + (i) * 4u)
#define NV_PVIDEO_POINT_OUT(i) (0x008948u + (i) * 4u)
#define NV_PVIDEO_SIZE_OUT(i)  (0x008950u + (i) * 4u)
#define NV_PVIDEO_FORMAT(i)    (0x008958u + (i) * 4u)

static int s_ovl_active;

static volatile uint32_t *guest32(uint32_t va)
{
    return (volatile uint32_t *)vvm_ptr(va + (uint32_t)g_xbox_mem_offset);
}
static uint32_t nv_reg(uint32_t reg) { return *guest32(NV_REG_BASE + reg); }

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* -> A8B8G8R8 como s_frame */
static uint32_t yuv_to_abgr(int y, int u, int v)
{
    int c = 298 * (y - 16), d = u - 128, e = v - 128;
    return 0xFF000000u | ((uint32_t)clamp8((c + 516 * d + 128) >> 8) << 16)
         | ((uint32_t)clamp8((c - 100 * d - 208 * e + 128) >> 8) << 8)
         |  (uint32_t)clamp8((c + 409 * e + 128) >> 8);
}

/* El cuadro de vídeo a su resolución (bytes RGBA, como s_frame) y dónde va. */
static uint32_t *s_ovl;
static uint32_t s_ovl_w, s_ovl_h, s_ovl_rect[4], s_ovl_serial;
static volatile int s_hw_mode;   /* el backend vitaGL tiene la pantalla */

static void overlay_latch(int buf)
{
    uint32_t size_in = nv_reg(NV_PVIDEO_SIZE_IN(buf)), format = nv_reg(NV_PVIDEO_FORMAT(buf));
    uint32_t w = size_in & 0x7FFu, h = (size_in >> 16) & 0x7FFu, pitch = format & 0xFFFFu;
    uint32_t addr = (nv_reg(NV_PVIDEO_BASE(buf)) + nv_reg(NV_PVIDEO_OFFSET(buf))) & 0x0FFFFFFFu;
    uint32_t po = nv_reg(NV_PVIDEO_POINT_OUT(buf)), so = nv_reg(NV_PVIDEO_SIZE_OUT(buf));
    uint32_t ox = po & 0xFFFu, oy = (po >> 16) & 0xFFFu, ow = so & 0xFFFu, oh = (so >> 16) & 0xFFFu;
    int yuy2 = (format >> 16) & 1;     /* 1: Y0 U Y1 V, 0: U Y0 V Y1 */
    uint32_t x, y;

    if (!w || !h || !ow || !oh || pitch < w * 2 || addr >= 0x04000000u) return;
    if (!s_ovl_active)
        fprintf(stderr, "[FB] overlay de vídeo: %ux%u %s en 0x%08X -> %ux%u+%u+%u\n",
                w, h, yuy2 ? "YUY2" : "UYVY", 0x80000000u | addr, ow, oh, ox, oy);
    s_ovl_active = 1;
    pthread_mutex_lock(&s_frame_lock);
    if (w != s_ovl_w || h != s_ovl_h || !s_ovl) {
        free(s_ovl);
        s_ovl = (uint32_t *)malloc((size_t)w * h * 4);
        s_ovl_w = s_ovl ? w : 0; s_ovl_h = s_ovl ? h : 0;
    }
    if (s_ovl) {
        for (y = 0; y < h; y++) {
            /* la base es física: la ventana contigua es su vista (una sola reserva) */
            const uint8_t *row = (const uint8_t *)guest32(0x80000000u | (addr + y * pitch));
            uint32_t *d = &s_ovl[y * w];
            for (x = 0; x + 1 < w; x += 2) {
                const uint8_t *p = row + x * 2;
                int u = yuy2 ? p[1] : p[0], v = yuy2 ? p[3] : p[2];
                d[x]     = yuv_to_abgr(yuy2 ? p[0] : p[1], u, v);
                d[x + 1] = yuv_to_abgr(yuy2 ? p[2] : p[3], u, v);
            }
        }
        s_ovl_rect[0] = ox; s_ovl_rect[1] = oy; s_ovl_rect[2] = ow; s_ovl_rect[3] = oh;
        s_ovl_serial++;
        if (!s_hw_mode) {   /* por software: escalado dentro del cuadro del juego */
            for (y = 0; y < oh && oy + y < SRC_H; y++) {
                const uint32_t *src = &s_ovl[(y * h / oh) * w];
                uint32_t *d = &s_frame[(oy + y) * SRC_W];
                for (x = 0; x < ow && ox + x < SRC_W; x++) d[ox + x] = src[x * w / ow];
            }
            s_frame_serial++;
        }
    }
    pthread_mutex_unlock(&s_frame_lock);
}

/* fb_present.c: el cuadro de vídeo para quien compone la pantalla (nv2a_hw_vita.inc). */
int xbox_FramebufferOverlayGet(const uint32_t **px, uint32_t *w, uint32_t *h, uint32_t rect[4], uint32_t *serial)
{
    if (!s_ovl_active || !s_ovl || !s_ovl_rect[2] || !s_ovl_rect[3]) return 0;
    *px = s_ovl; *w = s_ovl_w; *h = s_ovl_h;
    memcpy(rect, s_ovl_rect, sizeof s_ovl_rect);
    *serial = s_ovl_serial;
    return 1;
}

static void overlay_tick(void)
{
    volatile uint32_t *buffer = guest32(NV_REG_BASE + NV_PVIDEO_BUFFER);
    volatile uint32_t *stop = guest32(NV_REG_BASE + NV_PVIDEO_STOP);
    uint32_t use = *buffer;
    if (*stop & 1u) {
        *buffer = 0;
        *stop = 0;
        if (s_ovl_active) fprintf(stderr, "[FB] overlay de vídeo apagado\n");
        s_ovl_active = 0;
        return;
    }
    if (use & 0x01u)      overlay_latch(0);
    else if (use & 0x10u) overlay_latch(1);
    else                  s_ovl_active = 0;
}

/* Hilo principal: escala el último frame si hay uno nuevo. */
void vita_fb_pump(void)
{
    int y, x;
    if (g_xbox_mem_offset) overlay_tick();
    if (s_hw_mode)
        return;   /* vitaGL tiene la pantalla: repinta vita_gl_idle_tick, en el hilo de GL */
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

/* Hilo de GL (nv2a_ack_thread, vía vitaize.py): un juego que no hace flips (un
 * vídeo, una carga) se repinta desde aquí, como el hilo de ventana de
 * fb_present.c, a lo sumo cada 16 ms. */
void vita_gl_idle_tick(void)
{
    extern void nv2a_hw_idle_present(void);
    static uint32_t last;
    uint32_t now;
    if (!s_hw_mode) return;
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); now = (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000); }
    if (now - last < 16) return;
    last = now;
    nv2a_hw_idle_present();
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
void xbox_FramebufferLimitFrame(void) {}
int  xbox_FramebufferDeferPresent(void) { return 0; }
void xbox_FramebufferSetOptionsCallbacks(int (*panel)(const uint32_t **, uint32_t *, uint32_t *),
                                         int (*event)(void *, unsigned, uintptr_t, intptr_t))
{ (void)panel; (void)event; }
void xbox_FramebufferSetWowProvider(void (*provider)(char *, size_t)) { (void)provider; }
int  xbox_FramebufferDebugPanelGet(const uint32_t **pixels, uint32_t *width, uint32_t *height)
{ (void)pixels; (void)width; (void)height; return 0; }
/* hw_init() espera una ventana; en la Vita la pantalla entera hace de ventana */
void *xbox_FramebufferWindowHandle(void) { return (void *)s_screen; }
void xbox_FramebufferWindowHwMode(int on) { s_hw_mode = on; }
void xbox_FramebufferHostKey(int vk, int down) { (void)vk; (void)down; }
void xbox_FramebufferHostMouse(int dx, int dy) { (void)dx; (void)dy; }
void xbox_FramebufferToggleDebugPanel(void) {}
void resot_host_draw_text(uint32_t *bits, int w, int h, int x, int y, const char *text, uint32_t argb)
{ (void)bits; (void)w; (void)h; (void)x; (void)y; (void)text; (void)argb; }
