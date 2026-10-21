/* hosttest: ventana en vivo (SDL2 sobre X11 -> XQuartz en el Mac) y mando por
 * teclado. Solo si DISPLAY está definido; si no, psp2_fake.c sigue con PPM.
 *
 * Todo SDL vive en un hilo propio: sceDisplaySetFrameBuf solo publica el
 * puntero del framebuffer y sceCtrlPeekBufferPositive lee los botones.
 *
 * Teclas: flechas = cruceta, Z = ✕, X = ◯, A = □, S = △, Enter = START,
 * Espacio = SELECT, Q = L, W = R, I/J/K/L = stick izquierdo.
 */
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <psp2/ctrl.h>

#include "window_sdl.h"

static const uint32_t *volatile s_fb;
static volatile int s_w = 960, s_h = 544, s_pitch = 960;
static volatile unsigned s_buttons;
static volatile uint8_t s_lx = 128, s_ly = 128;
static int s_on = -1;

static void read_keys(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    unsigned b = 0;
    if (k[SDL_SCANCODE_UP])     b |= SCE_CTRL_UP;
    if (k[SDL_SCANCODE_DOWN])   b |= SCE_CTRL_DOWN;
    if (k[SDL_SCANCODE_LEFT])   b |= SCE_CTRL_LEFT;
    if (k[SDL_SCANCODE_RIGHT])  b |= SCE_CTRL_RIGHT;
    if (k[SDL_SCANCODE_Z])      b |= SCE_CTRL_CROSS;
    if (k[SDL_SCANCODE_X])      b |= SCE_CTRL_CIRCLE;
    if (k[SDL_SCANCODE_A])      b |= SCE_CTRL_SQUARE;
    if (k[SDL_SCANCODE_S])      b |= SCE_CTRL_TRIANGLE;
    if (k[SDL_SCANCODE_RETURN]) b |= SCE_CTRL_START;
    if (k[SDL_SCANCODE_SPACE])  b |= SCE_CTRL_SELECT;
    if (k[SDL_SCANCODE_Q])      b |= SCE_CTRL_LTRIGGER;
    if (k[SDL_SCANCODE_W])      b |= SCE_CTRL_RTRIGGER;
    s_buttons = b;
    s_lx = k[SDL_SCANCODE_J] ? 0 : k[SDL_SCANCODE_L] ? 255 : 128;
    s_ly = k[SDL_SCANCODE_I] ? 0 : k[SDL_SCANCODE_K] ? 255 : 128;
}

static void *window_main(void *unused)
{
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    (void)unused;
    /* sobre X11 remoto: render por software, sin GLX */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[HOSTTEST] sin ventana: %s\n", SDL_GetError());
        return NULL;
    }
    win = SDL_CreateWindow("hosttest - PS Vita 960x544", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           960, 544, SDL_WINDOW_RESIZABLE);
    ren = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 960, 544) : NULL;
    if (!tex) {
        fprintf(stderr, "[HOSTTEST] sin ventana: %s\n", SDL_GetError());
        return NULL;
    }
    fprintf(stderr, "[HOSTTEST] ventana abierta en %s\n", getenv("DISPLAY"));
    for (;;) {
        SDL_Event e;
        const uint32_t *fb = s_fb;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) exit(0);
        read_keys();
        if (fb) {
            SDL_UpdateTexture(tex, NULL, fb, s_pitch * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }
        SDL_Delay(33);   /* ~30 Hz: por X11 remoto no da para más */
    }
    return NULL;
}

int hosttest_window_on(void)
{
    if (s_on < 0) {
        pthread_t t;
        s_on = getenv("DISPLAY") && pthread_create(&t, NULL, window_main, NULL) == 0;
    }
    return s_on;
}

void hosttest_window_frame(const uint32_t *abgr, int w, int h, int pitch)
{
    if (!hosttest_window_on()) return;
    s_w = w; s_h = h; s_pitch = pitch;
    s_fb = abgr;
}

void hosttest_window_pad(unsigned *buttons, uint8_t *lx, uint8_t *ly)
{
    if (!hosttest_window_on()) return;
    *buttons |= s_buttons;
    *lx = s_lx;
    *ly = s_ly;
}
