/* hosttest: lo de vitaGL que usa src/nv2a_hw_vita.inc, sobre OpenGL de Mesa
 * (llvmpipe, perfil de compatibilidad) en un pbuffer EGL sin pantalla. Los
 * shaders llegan en GLSL (RESOT_HW_GLSL). vglSwapBuffers lee el cuadro y lo
 * entrega por sceDisplaySetFrameBuf (psp2_fake.c: ventana y capturas PPM).
 *
 * El contexto EGL es de un hilo a la vez: nv2a_vita_glthread.h lo toma al
 * entrar en s_hw.lock y lo suelta al salir (hosttest_gl_acquire/release). */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/display.h>

static EGLDisplay s_dpy;
static EGLSurface s_surf;
static EGLContext s_ctx;
static int s_w, s_h;
static uint32_t *s_pixels, *s_frame;
static __thread int t_depth, t_current;

GLboolean vglInitExtended(int legacy_pool_size, int width, int height, int ram_threshold, int msaa)
{
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    static const EGLint ctx_attr[] = { EGL_NONE };
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLConfig cfg;
    EGLint n, surf_attr[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    (void)legacy_pool_size; (void)ram_threshold; (void)msaa;
    s_dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    if (s_dpy == EGL_NO_DISPLAY || !eglInitialize(s_dpy, NULL, NULL)
            || !eglChooseConfig(s_dpy, cfg_attr, &cfg, 1, &n) || !n || !eglBindAPI(EGL_OPENGL_API)
            || (s_surf = eglCreatePbufferSurface(s_dpy, cfg, surf_attr)) == EGL_NO_SURFACE
            || (s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr)) == EGL_NO_CONTEXT
            || !eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx)) {
        fprintf(stderr, "[HOSTTEST] EGL/GL no disponible (error 0x%X)\n", eglGetError());
        return GL_FALSE;
    }
    t_current = 1;
    s_w = width; s_h = height;
    s_pixels = malloc((size_t)width * height * 4);
    s_frame = malloc((size_t)width * height * 4);
    fprintf(stderr, "[HOSTTEST] GL %s (%s) en pbuffer %dx%d\n", glGetString(GL_VERSION),
            glGetString(GL_RENDERER), width, height);
    return GL_TRUE;
}

void hosttest_gl_acquire(void)
{
    t_depth++;
    if (!t_current && s_ctx) {
        if (!eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx)) {
            fprintf(stderr, "[HOSTTEST] eglMakeCurrent: 0x%X\n", eglGetError());
            abort();
        }
        t_current = 1;
    }
}

void hosttest_gl_release(void)
{
    if (t_depth > 0) t_depth--;
    if (!t_depth && t_current) {
        eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        t_current = 0;
    }
}

void vglSwapBuffers(GLboolean has_commondialog)
{
    SceDisplayFrameBuf fb = { sizeof fb };
    GLint bound = 0;
    int y;
    (void)has_commondialog;
    /* la pantalla es el framebuffer 0, no el FBO que haya enlazado ahora */
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, s_w, s_h, GL_RGBA, GL_UNSIGNED_BYTE, s_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound);
    for (y = 0; y < s_h; y++)   /* glReadPixels: la fila 0 es la de abajo */
        memcpy(&s_frame[y * s_w], &s_pixels[(s_h - 1 - y) * s_w], (size_t)s_w * 4);
    {
        static time_t last;
        if (time(NULL) - last >= 5) {
            const uint8_t *c = (const uint8_t *)&s_frame[(s_h / 2) * s_w + s_w / 2];
            last = time(NULL);
            fprintf(stderr, "[HOSTTEST] swap: centro %02X%02X%02X\n", c[0], c[1], c[2]);
        }
    }
    fb.base = s_frame; fb.pitch = (unsigned)s_w; fb.width = (unsigned)s_w; fb.height = (unsigned)s_h;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    eglSwapBuffers(s_dpy, s_surf);
}
