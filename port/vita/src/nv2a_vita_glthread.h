/* reSOT Vita: dónde se puede llamar a GL (lo incluye nv2a_pb_exec.c, vía
 * vitaize, justo antes de nv2a_hw_d3d11.inc).
 *
 * Upstream llama al encoder desde varios hilos (ejecutor NV2A, repintado de
 * vídeos), siempre dentro de s_hw.lock. vitaGL no tiene contexto por hilo,
 * así que en la Vita basta ese lock. En hosttest (Mesa/EGL) el contexto se
 * hace actual en el hilo que entra en s_hw.lock y se suelta al salir. */
#ifndef RESOT_VITA_GLTHREAD_H
#define RESOT_VITA_GLTHREAD_H
#if defined(RESOT_HW_GLSL)
void hosttest_gl_acquire(void);
void hosttest_gl_release(void);
#define VG_GL_ENTER() hosttest_gl_acquire()
#define VG_GL_LEAVE() hosttest_gl_release()
#define EnterCriticalSection(cs) \
    ((EnterCriticalSection)(cs), ((cs) == &s_hw.lock ? hosttest_gl_acquire() : (void)0))
#define LeaveCriticalSection(cs) \
    (((cs) == &s_hw.lock ? hosttest_gl_release() : (void)0), (LeaveCriticalSection)(cs))
#else
#define VG_GL_ENTER() ((void)0)
#define VG_GL_LEAVE() ((void)0)
#endif
#endif
