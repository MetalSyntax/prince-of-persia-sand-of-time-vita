/**
 * reSOT Vita - mando: el estado del pad de Xbox que pide el runtime
 * (src/input/xinput_xbox.h), como port/android/src/input_android.c.
 *
 *   ✕ A saltar      ◯ B cancelar/soltar   □ X espada        △ Y daga
 *   L1 gatillo izq. (rebobinar)           R1 gatillo der. (especial)
 *   stick izq. mover  stick der. cámara   START pausa  SELECT back
 *   táctil trasero: mitad izq. = Black (vista alt.), mitad der. = White (mirar)
 *
 * Todo termina en el filtro de las opciones (options_android.c), como en
 * Windows/Android, que aplica las asignaciones del usuario.
 */
#include <stdint.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include "platform/win32_compat.h"
#include "xinput_xbox.h"

static void (*s_host_filter)(void *, unsigned);
static DWORD s_packet;

void xbox_InputSetHostFilter(void (*filter)(void *, unsigned)) { s_host_filter = filter; }

void vita_input_init(void)
{
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
}

void xbox_InputInit(void) {}

/* 0..255 con centro en 128 -> -32767..32767, con zona muerta. */
static SHORT axis(uint8_t v, int invert)
{
    int a = ((int)v - 128) * 258;
    if (a > -3000 && a < 3000) a = 0;
    if (a > 32767) a = 32767;
    if (a < -32767) a = -32767;
    return (SHORT)(invert ? -a : a);
}

DWORD xbox_InputGetState(DWORD port, XBOX_INPUT_STATE *state)
{
    SceCtrlData pad;
    SceTouchData touch;
    XBOX_GAMEPAD *g;
    int i;
    if (port != 0 || !state)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(state, 0, sizeof *state);
    g = &state->Gamepad;
    sceCtrlPeekBufferPositive(0, &pad, 1);

    if (pad.buttons & SCE_CTRL_UP)     g->wButtons |= XBOX_GAMEPAD_DPAD_UP;
    if (pad.buttons & SCE_CTRL_DOWN)   g->wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
    if (pad.buttons & SCE_CTRL_LEFT)   g->wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
    if (pad.buttons & SCE_CTRL_RIGHT)  g->wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (pad.buttons & SCE_CTRL_START)  g->wButtons |= XBOX_GAMEPAD_START;
    if (pad.buttons & SCE_CTRL_SELECT) g->wButtons |= XBOX_GAMEPAD_BACK;
    g->bAnalogButtons[XBOX_BUTTON_A]        = (pad.buttons & SCE_CTRL_CROSS)    ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_B]        = (pad.buttons & SCE_CTRL_CIRCLE)   ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_X]        = (pad.buttons & SCE_CTRL_SQUARE)   ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_Y]        = (pad.buttons & SCE_CTRL_TRIANGLE) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_LTRIGGER] = (pad.buttons & SCE_CTRL_LTRIGGER) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_RTRIGGER] = (pad.buttons & SCE_CTRL_RTRIGGER) ? 255 : 0;
    g->sThumbLX = axis(pad.lx, 0);
    g->sThumbLY = axis(pad.ly, 1);   /* Xbox: arriba es + */
    g->sThumbRX = axis(pad.rx, 0);
    g->sThumbRY = axis(pad.ry, 1);

    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) > 0) {
        for (i = 0; i < (int)touch.reportNum; i++) {
            /* el panel trasero mide 1920 de ancho */
            if (touch.report[i].x < 960) g->bAnalogButtons[XBOX_BUTTON_BLACK] = 255;
            else                         g->bAnalogButtons[XBOX_BUTTON_WHITE] = 255;
        }
    }

    state->dwPacketNumber = ++s_packet;
    if (s_host_filter)
        s_host_filter(state, (unsigned)port);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD port, const XBOX_VIBRATION *v)
{
    (void)v;   /* la Vita no vibra */
    return port == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL xbox_InputIsConnected(DWORD port) { return port == 0; }

DWORD xbox_InputGetCapabilities(DWORD port, DWORD flags, XBOX_INPUT_CAPABILITIES *caps)
{
    (void)flags;
    if (!caps || port != 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(caps, 0, sizeof *caps);
    caps->Type = 1;
    caps->SubType = 1;
    return ERROR_SUCCESS;
}

/* ✕ o START saltan un vídeo (los hooks del target), como Espacio/A en Windows. */
int resot_movie_skip_down(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    return (pad.buttons & (SCE_CTRL_CROSS | SCE_CTRL_START)) != 0;
}
