/*
 * Smoke test de Fase 0. Comprueba en la Vita real (o Vita3K):
 *   1. que el VPK instala y arranca,
 *   2. que ur0:data/libshacccg.suprx existe (sin él vitaGL deja la pantalla en negro),
 *   3. que los archivos de data_manifest.txt están en DATA_DIR.
 * Pantalla verde = todo OK, roja = falta algo. Detalle en DATA_DIR/<slug>.log. START sale.
 */
#include <stdio.h>
#include <stdarg.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>
#include "manifest.h"

static FILE *g_log;

static void logf_(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (g_log) { vfprintf(g_log, fmt, ap); fputc('\n', g_log); fflush(g_log); }
  va_end(ap);
}

static int exists(const char *path) {
  SceIoStat st;
  return sceIoGetstat(path, &st) >= 0;
}

int main(void) {
  sceIoMkdir(DATA_DIR, 0777);
  g_log = fopen(DATA_DIR "/" APP_SLUG ".log", "w");
  logf_("[smoke] %s — DATA_DIR=%s", APP_SLUG, DATA_DIR);

  int missing = 0;
  if (!exists("ur0:data/libshacccg.suprx")) {
    logf_("[FALTA] ur0:data/libshacccg.suprx (instalar ShaRKF00D y correrlo una vez)");
    sceKernelExitProcess(1); /* sin el compilador de shaders vitaGL no puede inicializar */
  }
  for (int i = 0; kRequired[i]; i++) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", DATA_DIR, kRequired[i]);
    int ok = exists(path);
    missing += !ok;
    logf_("[%s] %s", ok ? "OK" : "FALTA", path);
  }
  logf_("[smoke] faltan %d archivo(s)", missing);

  vglInit(0x800000);
  for (;;) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) break;
    if (missing) glClearColor(0.6f, 0.0f, 0.0f, 1.0f);
    else         glClearColor(0.0f, 0.5f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    vglSwapBuffers(GL_FALSE);
  }
  vglEnd();
  if (g_log) fclose(g_log);
  sceKernelExitProcess(0);
  return 0;
}
