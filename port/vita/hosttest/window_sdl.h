/* hosttest: ventana SDL2 en vivo (ver window_sdl.c). */
#ifndef HOSTTEST_WINDOW_SDL_H
#define HOSTTEST_WINDOW_SDL_H
#include <stdint.h>

int  hosttest_window_on(void);
void hosttest_window_frame(const uint32_t *abgr, int w, int h, int pitch);
void hosttest_window_pad(unsigned *buttons, uint8_t *lx, uint8_t *ly);

#endif
