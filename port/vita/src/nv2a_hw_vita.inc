/* reSOT Vita: encoder GPU del ejecutor NV2A sobre vitaGL (V1.1).
 *
 * Sustituye a upstream/.../kernel/nv2a_hw_native.inc (SDL GPU/Vulkan de
 * Android): la misma interfaz ng_*, incluida por la copia vitaizada de
 * nv2a_hw_d3d11.inc, que aporta el estado NV2A, las superficies por offset
 * guest, el decodificador de texturas y el despacho (hw_draw_batch, hw_clear,
 * hw_flip...). Como en Android, los vertex programs corren en la CPU
 * (nv2a_vsh_interp.c) y aquí llegan vértices ya transformados (HwVertex).
 *
 * Diferencia con upstream: el uber shader que interpreta los register
 * combiners por píxel (s_hw_hlsl, ps_main) sería demasiado caro en la GPU de
 * la Vita, así que cada estado de combiners distinto genera su propio
 * fragment shader con los control words ya resueltos (vg_ps_source). Se
 * compila una vez (vitaGL: CG por SceShaccCg; hosttest: GLSL 1.20 en Mesa).
 *
 * Convención de filas: la fila 0 de toda textura (decodificada o render
 * target) es la fila 0 del guest. El vertex shader decide hacia qué lado de
 * NDC va el guest y = 0 (vp.w); RECOMP_VITA_RT_FLIP=0/1 lo fuerza si en la
 * consola sale invertido.
 *
 * Huecos conocidos (además de los de upstream: stencil, bump-env, puntos y
 * líneas): sin color de mezcla constante (vitaGL no tiene glBlendColor) y la
 * visibilidad (occlusion) se aproxima con el área dibujada, sin oclusión.
 */
#include <stdarg.h>

#if defined(RESOT_HW_GLSL)
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
GLboolean vglInitExtended(int legacy_pool_size, int width, int height, int ram_threshold, int msaa);
void vglSwapBuffers(GLboolean has_commondialog);
#define VG_MSAA_NONE 0
#define VG_RT_FLIP_DEFAULT 0        /* Mesa: GL estándar */
#define VG_VERTEX_SHADER   GL_VERTEX_SHADER
#define VG_FRAGMENT_SHADER GL_FRAGMENT_SHADER
#else
#include <vitaGL.h>
#define VG_MSAA_NONE SCE_GXM_MULTISAMPLE_NONE
#define VG_RT_FLIP_DEFAULT 1        /* hipótesis: vitaGL guarda los FBO de arriba abajo */
#define VG_VERTEX_SHADER   GL_CG_VERTEX_SHADER_EXT
#define VG_FRAGMENT_SHADER GL_CG_FRAGMENT_SHADER_EXT
#endif

#define VG_SCREEN_W 960
#define VG_SCREEN_H 544
#define VG_MAX_PROGRAMS 512

typedef struct {
    GLuint fbo, tex, depth;            /* depth: renderbuffer enganchado ahora */
    uint32_t offset, w, h, sc, frame;
} VgSurface;
typedef struct { GLuint rb; uint32_t offset, w, h; } VgDepth;
typedef struct {
    GLuint tex;
    Texture guest;
    uint32_t hash, checked, used, levels;
    GLint wrap_s, wrap_t, min, mag;    /* parámetros ya puestos */
} VgTexture;

/* Lo que fija el código de un fragment shader; el resto son uniforms. */
typedef struct {
    uint32_t cw[8][4];                 /* color/alpha ICW y OCW de las etapas usadas */
    uint32_t control, sp, final0, final1, cpm;
    uint32_t rc_seen, atest_func;      /* atest_func 0 = sin alpha test */
    uint32_t bound, cube;              /* bit i: etapa i con textura / cúbica */
    uint32_t vsp;                      /* vertex shader: 0 fijo (CPU), si no índice+1 en s_vg.vsp */
} VgPsKey;
typedef struct VgProgram {
    VgPsKey key;
    uint32_t hash;
    GLuint prog;
    GLint u_vp, u_ws, u_fac0, u_fac1, u_finc0, u_finc1, u_fogc, u_tscale, u_aref, u_tex[4];
    GLint u_c, u_fogp;
    uint32_t cgen;                     /* generación de constantes ya subida */
    struct VgProgram *next;
} VgProgram;

/* Un vertex program del NV2A traducido (nv2a_vsh_emit_hlsl) a CG/GLSL. */
#define VG_MAX_VSP 256
typedef struct { uint32_t key, ninputs; uint8_t inputs[8]; GLuint vs; } VgVsp;

static struct {
    int active, ysign, gpu_vsh;
    VgVsp vsp[VG_MAX_VSP];
    uint32_t nvsp;
    VgSurface surfaces[HW_MAX_SURFACES], *front, *largest;
    VgDepth depths[HW_MAX_DEPTHS];
    VgTexture textures[HW_MAX_TEXTURES];
    VgProgram *programs[VG_MAX_PROGRAMS];
    uint32_t nprograms;
    GLuint white, white_cube, blit, overlay;
    GLint blit_tex;
    uint32_t overlay_w, overlay_h, overlay_serial;
    int applied_percent;
    /* visibilidad: área dibujada (píxeles guest) mientras está activa */
    int vis_enabled;
    double vis_pixels;
} s_vg;

static int ng_active(void) { return s_vg.active; }

static void vg_fail(const char *what)
{
    fprintf(stderr, "[HW vita] fatal: %s (GL 0x%X)\n", what, glGetError());
    fflush(stderr);
    abort();   /* como upstream: nunca perder dibujos del guest en silencio */
}

/* ── shaders ─────────────────────────────────────────────── */

typedef struct { char *p; size_t n, cap; } VgBuf;

static void vg_emit(VgBuf *b, const char *fmt, ...)
{
    va_list ap;
    int need;
    for (;;) {
        va_start(ap, fmt);
        need = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (need >= 0 && b->n + (size_t)need < b->cap) { b->n += (size_t)need; return; }
        b->cap = (b->cap ? b->cap * 2 : 8192) + (size_t)need;
        if (!(b->p = (char *)realloc(b->p, b->cap))) vg_fail("shader buffer");
    }
}

/* Lo que difiere entre CG (vitaGL) y GLSL 1.20 (hosttest); el cuerpo es común. */
#if defined(RESOT_HW_GLSL)
static const char s_vg_prelude[] =
    "#version 120\n"
    "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n"
    "#define saturate(x) clamp(x, 0.0, 1.0)\n#define tex2D texture2D\n#define texCUBE textureCube\n"
    "#define FMOD mod\n";
#else
static const char s_vg_prelude[] = "#define FMOD fmod\n";
#endif

static const char s_vg_vs[] =
#if defined(RESOT_HW_GLSL)
    "attribute float4 a_pos, a_d0, a_d1, a_fog, a_t0, a_t1, a_t2, a_t3;\n"
    "uniform float4 vp, ws;\n"
    "varying float4 v_d0, v_d1, v_fog, v_t0, v_t1, v_t2, v_t3;\n"
    "void main() {\n"
#else
    "void main(float4 a_pos, float4 a_d0, float4 a_d1, float4 a_fog,\n"
    "  float4 a_t0, float4 a_t1, float4 a_t2, float4 a_t3, uniform float4 vp, uniform float4 ws,\n"
    "  float4 out gl_Position : POSITION, float4 out v_t0 : TEXCOORD0, float4 out v_t1 : TEXCOORD1,\n"
    "  float4 out v_t2 : TEXCOORD2, float4 out v_t3 : TEXCOORD3, float4 out v_d0 : TEXCOORD4,\n"
    "  float4 out v_d1 : TEXCOORD5, float4 out v_fog : TEXCOORD6) {\n"
#endif
    /* vs_finish de upstream (s_hw_hlsl_vs_common): a_pos ya en píxeles de la
     * superficie, z del depth buffer y w de clip. vp.w = lado de NDC del guest y=0. */
    "  float w = a_pos.w; float x = a_pos.x;\n"
    "  bool flat2d = abs(w - 1.0) < 0.0001;\n"
    "  if (ws.x != 1.0 && flat2d && x > ws.z && x < ws.y - ws.z) x = (x - 0.5 * ws.y) * ws.x + 0.5 * ws.y;\n"
    "  if (flat2d) x += ws.w;\n"
    "  gl_Position = float4((x * vp.x - 1.0) * w, vp.w * (a_pos.y * vp.y - 1.0) * w,\n"
    "                       (2.0 * saturate(a_pos.z * vp.z) - 1.0) * w, w);\n"
    "  v_d0 = saturate(a_d0); v_d1 = saturate(a_d1); v_fog = a_fog;\n"
    "  v_t0 = a_t0; v_t1 = a_t1; v_t2 = a_t2; v_t3 = a_t3;\n"
    "}\n";

/* Lectura de un registro de combiner (r0 lee cero) con su mapeo de entrada. */
static void vg_map_in(char *out, size_t cap, const char *v, uint32_t b, int vec)
{
    const char *z = vec ? "float3(0.0, 0.0, 0.0)" : "0.0";
    switch ((b >> 5) & 7) {
    case 0: snprintf(out, cap, "max(%s, %s)", v, z); break;
    case 1: snprintf(out, cap, "(1.0 - saturate(%s))", v); break;
    case 2: snprintf(out, cap, "(2.0 * max(%s, %s) - 1.0)", v, z); break;
    case 3: snprintf(out, cap, "(1.0 - 2.0 * max(%s, %s))", v, z); break;
    case 4: snprintf(out, cap, "(max(%s, %s) - 0.5)", v, z); break;
    case 5: snprintf(out, cap, "(0.5 - max(%s, %s))", v, z); break;
    case 6: snprintf(out, cap, "%s", v); break;
    default: snprintf(out, cap, "(-%s)", v); break;
    }
}
static void vg_rd_rgb(char *out, size_t cap, uint32_t b)
{
    char v[16];
    snprintf(v, sizeof v, (b & 0x10) ? "r%u.www" : "r%u.xyz", b & 15);
    vg_map_in(out, cap, v, b, 1);
}
static void vg_rd_a(char *out, size_t cap, uint32_t b)
{
    char v[16];
    snprintf(v, sizeof v, (b & 0x10) ? "r%u.w" : "r%u.z", b & 15);   /* alpha lee el azul */
    vg_map_in(out, cap, v, b, 0);
}
static const char *vg_map_out(uint32_t m)
{
    switch (m) {
    case 0x08: return "(%s - 0.5)";
    case 0x10: return "(%s * 2.0)";
    case 0x18: return "((%s - 0.5) * 2.0)";
    case 0x20: return "(%s * 4.0)";
    case 0x30: return "(%s * 0.5)";
    default:   return "(%s)";
    }
}

/* Fragment shader de un estado: ps_main de upstream con los control words resueltos. */
static char *vg_ps_source(const VgPsKey *k)
{
    VgBuf b = {0};
    char A[96], B[96], C[96], D[96], e[96], t[64];
    uint32_t i, s, n = k->control & 0xFF, flags = k->control >> 8;
    if (n > 8) n = 8;
    vg_emit(&b, "%s", s_vg_prelude);
#if defined(RESOT_HW_GLSL)
    vg_emit(&b, "varying float4 v_d0, v_d1, v_fog, v_t0, v_t1, v_t2, v_t3;\n"
                "uniform float4 fac0[8], fac1[8], finc0, finc1, fogc, tscale[4];\nuniform float aref;\n");
    for (i = 0; i < 4; i++)
        vg_emit(&b, "uniform %s tex%u;\n", (k->cube >> i) & 1 ? "samplerCube" : "sampler2D", i);
    vg_emit(&b, "void main() {\n");
#else
    vg_emit(&b, "float4 main(float4 v_t0 : TEXCOORD0, float4 v_t1 : TEXCOORD1, float4 v_t2 : TEXCOORD2,\n"
                "  float4 v_t3 : TEXCOORD3, float4 v_d0 : TEXCOORD4, float4 v_d1 : TEXCOORD5,\n"
                "  float4 v_fog : TEXCOORD6, uniform float4 fac0[8], uniform float4 fac1[8],\n"
                "  uniform float4 finc0, uniform float4 finc1, uniform float4 fogc,\n"
                "  uniform float4 tscale[4], uniform float aref");
    for (i = 0; i < 4; i++)
        vg_emit(&b, ",\n  uniform %s tex%u", (k->cube >> i) & 1 ? "samplerCUBE" : "sampler2D", i);
    vg_emit(&b, ") : COLOR {\n");
#endif
    /* texturas de las cuatro etapas */
    for (i = 0; i < 4; i++) {
        uint32_t mode = k->rc_seen ? (k->sp >> (5 * i)) & 31 : (i == 0 ? 1 : 0);
        if ((k->bound >> i) & 1) {
            if ((k->cube >> i) & 1) vg_emit(&b, "  float4 s%u = texCUBE(tex%u, v_t%u.xyz);\n", i, i, i);
            else vg_emit(&b, "  float4 s%u = tex2D(tex%u, v_t%u.xy / (v_t%u.w != 0.0 ? v_t%u.w : 1.0) * tscale[%u].xy);\n",
                         i, i, i, i, i, i);
        } else vg_emit(&b, "  float4 s%u = float4(1.0, 1.0, 1.0, 1.0);\n", i);
        if (!k->rc_seen) continue;
        /* stage_tex(): lo que la etapa deja en su registro (r8..r11) */
        if (mode == 0) vg_emit(&b, "  float4 r%u = float4(0.0, 0.0, 0.0, 1.0);\n", 8 + i);
        else if (mode <= 3) vg_emit(&b, "  float4 r%u = s%u;\n", 8 + i, i);
        else if (mode == 4) vg_emit(&b, "  float4 r%u = saturate(v_t%u);\n", 8 + i, i);
        else {
            if (mode == 5) {
                uint32_t j;
                static const char comp[4] = {'x', 'y', 'z', 'w'};
                for (j = 0; j < 4; j++)
                    vg_emit(&b, "  if (v_t%u.%c %s 0.0) discard;\n", i, comp[j],
                            ((k->cpm >> (4 * i + j)) & 1) ? ">=" : "<");
            }
            vg_emit(&b, "  float4 r%u = float4(0.0, 0.0, 0.0, 0.0);\n", 8 + i);
        }
    }
    vg_emit(&b, "  float4 o;\n");
    if (!k->rc_seen) {
        vg_emit(&b, "  o = v_d0 * s0;\n");
    } else {
        vg_emit(&b, "  float4 r0 = float4(0.0, 0.0, 0.0, 0.0), r1 = r0, r2 = r0, r6 = r0, r7 = r0;\n"
                    "  float4 r13 = r0, r14 = r0, r15 = r0;\n"
                    "  float4 r3 = float4(fogc.xyz, v_fog.x), r4 = v_d0, r5 = v_d1;\n"
                    "  float4 r12 = float4(0.0, 0.0, 0.0, %s);\n", (k->sp & 31) ? "r8.w" : "1.0");
        for (s = 0; s < n; s++) {
            uint32_t icw = k->cw[s][0], aicw = k->cw[s][1], ocw = k->cw[s][2], aocw = k->cw[s][3];
            uint32_t fl = ocw >> 12, afl = aocw >> 12, d;
            vg_emit(&b, "  {\n    r1 = fac0[%u]; r2 = fac1[%u];\n",
                    (flags & 0x010) ? s : 0, (flags & 0x100) ? s : 0);
            if ((fl & 4) || (afl & 4))
                vg_emit(&b, "    bool mux = %s;\n", (flags & 1) ? "r12.w >= 0.5"
                                                   : "FMOD(floor(r12.w * 255.0), 2.0) >= 1.0");
            vg_rd_rgb(A, sizeof A, icw >> 24); vg_rd_rgb(B, sizeof B, icw >> 16);
            vg_rd_rgb(C, sizeof C, icw >> 8);  vg_rd_rgb(D, sizeof D, icw);
            vg_emit(&b, "    float3 A = %s, B = %s, C = %s, D = %s;\n", A, B, C, D);
            vg_rd_a(A, sizeof A, aicw >> 24); vg_rd_a(B, sizeof B, aicw >> 16);
            vg_rd_a(C, sizeof C, aicw >> 8);  vg_rd_a(D, sizeof D, aicw);
            vg_emit(&b, "    float aA = %s, aB = %s, aC = %s, aD = %s;\n", A, B, C, D);
            vg_emit(&b, "    float3 pab = %s;\n", (fl & 2) ? "dot(A, B) * float3(1.0, 1.0, 1.0)" : "A * B");
            vg_emit(&b, "    float3 pcd = %s;\n", (fl & 1) ? "dot(C, D) * float3(1.0, 1.0, 1.0)" : "C * D");
            vg_emit(&b, "    float3 sum = %s;\n", (fl & 4) ? "(mux ? pcd : pab)" : "pab + pcd");
            vg_emit(&b, "    float qab = aA * aB, qcd = aC * aD;\n");
            vg_emit(&b, "    float qsum = %s;\n", (afl & 4) ? "(mux ? qcd : qab)" : "qab + qcd");
#define VG_OUT(name, expr, m, scalar) do { \
                snprintf(t, sizeof t, vg_map_out(m), expr); \
                vg_emit(&b, scalar ? "    float %s = clamp(%s, -1.0, 1.0);\n" \
                                   : "    float3 %s = clamp(%s, -1.0, 1.0);\n", name, t); } while (0)
            VG_OUT("ab", "pab", fl & 0x38, 0); VG_OUT("cd", "pcd", fl & 0x38, 0); VG_OUT("ms", "sum", fl & 0x38, 0);
            VG_OUT("aab", "qab", afl & 0x38, 1); VG_OUT("acd", "qcd", afl & 0x38, 1); VG_OUT("ams", "qsum", afl & 0x38, 1);
#undef VG_OUT
            /* escrituras, después de todas las lecturas y en el orden de upstream */
            if ((d = (ocw >> 4) & 15)) vg_emit(&b, (fl & 0x80) ? "    r%u = float4(ab, ab.z);\n" : "    r%u.xyz = ab;\n", d);
            if ((d = ocw & 15))        vg_emit(&b, (fl & 0x40) ? "    r%u = float4(cd, cd.z);\n" : "    r%u.xyz = cd;\n", d);
            if ((d = (ocw >> 8) & 15)) vg_emit(&b, "    r%u.xyz = ms;\n", d);
            if ((d = (aocw >> 4) & 15)) vg_emit(&b, "    r%u.w = aab;\n", d);
            if ((d = aocw & 15))        vg_emit(&b, "    r%u.w = acd;\n", d);
            if ((d = (aocw >> 8) & 15)) vg_emit(&b, "    r%u.w = ams;\n", d);
            vg_emit(&b, "  }\n");
        }
        if (k->final0 || k->final1) {
            uint32_t f0 = k->final0, f1 = k->final1, ff = f1 & 0xFF;
            vg_emit(&b, "  r1 = finc0; r2 = finc1;\n");
            vg_emit(&b, "  {\n    float3 fa = %s, fb = %s;\n", (ff & 0x40) ? "1.0 - r5.xyz" : "r5.xyz",
                    (ff & 0x20) ? "1.0 - r12.xyz" : "r12.xyz");
            vg_emit(&b, "    r14 = float4(%s, 0.0);\n", (ff & 0x80) ? "saturate(fa + fb)" : "fa + fb");
            vg_rd_rgb(A, sizeof A, f1 >> 24); vg_rd_rgb(B, sizeof B, f1 >> 16);
            vg_emit(&b, "    r15 = float4(%s * %s, 0.0);\n", A, B);
            vg_rd_rgb(A, sizeof A, f0 >> 24); vg_rd_rgb(B, sizeof B, f0 >> 16);
            vg_rd_rgb(C, sizeof C, f0 >> 8);  vg_rd_rgb(D, sizeof D, f0);
            vg_emit(&b, "    float3 A = %s, B = %s, C = %s, D = %s;\n", A, B, C, D);
            vg_rd_a(e, sizeof e, f1 >> 8);
            vg_emit(&b, "    o = float4(D + C * (1.0 - A) + B * A, %s);\n  }\n", e);
        } else vg_emit(&b, "  o = r12;\n");
    }
    vg_emit(&b, "  o = saturate(o);\n");
    if (k->atest_func && k->atest_func != 0x207) {
        static const char *ok[] = {"false", "v < aref", "v == aref", "v <= aref", "v > aref", "v != aref", "v >= aref"};
        uint32_t f = k->atest_func - 0x200;
        vg_emit(&b, "  { float v = floor(o.w * 255.0 + 0.5); if (!(%s)) discard; }\n", f < 7 ? ok[f] : "true");
    }
#if defined(RESOT_HW_GLSL)
    vg_emit(&b, "  gl_FragColor = o;\n}\n");
#else
    vg_emit(&b, "  return o;\n}\n");
#endif
    return b.p;
}

static GLuint vg_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0, len = (GLint)strlen(src);
    glShaderSource(sh, 1, &src, &len);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "[HW vita] shader rechazado:\n%s\n--- fuente ---\n%s\n", log, src);
        fflush(stderr);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint vg_link(GLuint vs, GLuint fs, const char *const *attrs, int nattr)
{
    GLuint p = glCreateProgram();
    GLint ok = 0;
    int i;
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    for (i = 0; i < nattr; i++) glBindAttribLocation(p, (GLuint)i, attrs[i]);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "[HW vita] programa rechazado: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static const char *const s_vg_attrs[8] = {"a_pos", "a_d0", "a_d1", "a_fog", "a_t0", "a_t1", "a_t2", "a_t3"};
static const char *const s_vg_prog_attrs[8] = {"a_in0", "a_in1", "a_in2", "a_in3", "a_in4", "a_in5", "a_in6", "a_in7"};

static VgProgram *vg_program(const VgPsKey *k)
{
    static GLuint vs;
    GLuint vsx;
    const uint8_t *p = (const uint8_t *)k;
    uint32_t h = 2166136261u, i;
    VgProgram *e;
    GLuint fs;
    char *src, name[16];
    for (i = 0; i < sizeof *k; i++) h = (h ^ p[i]) * 16777619u;
    for (e = s_vg.programs[h % VG_MAX_PROGRAMS]; e; e = e->next)
        if (e->hash == h && !memcmp(&e->key, k, sizeof *k)) return e;
    if (!vs) {
        char *v = (char *)malloc(strlen(s_vg_prelude) + sizeof s_vg_vs);
        strcpy(v, s_vg_prelude); strcat(v, s_vg_vs);
        vs = vg_shader(VG_VERTEX_SHADER, v);
        free(v);
        if (!vs) vg_fail("vertex shader");
    }
    vsx = k->vsp ? s_vg.vsp[k->vsp - 1].vs : vs;
    src = vg_ps_source(k);
    fs = vg_shader(VG_FRAGMENT_SHADER, src);
    free(src);
    e = (VgProgram *)calloc(1, sizeof *e);
    if (!e) vg_fail("program allocation");
    e->key = *k; e->hash = h;
    e->prog = fs ? vg_link(vsx, fs, k->vsp ? s_vg_prog_attrs : s_vg_attrs, 8) : 0;
    if (fs) glDeleteShader(fs);
    if (e->prog) {
        e->u_vp = glGetUniformLocation(e->prog, "vp");
        e->u_ws = glGetUniformLocation(e->prog, "ws");
        e->u_fac0 = glGetUniformLocation(e->prog, "fac0");
        e->u_fac1 = glGetUniformLocation(e->prog, "fac1");
        e->u_finc0 = glGetUniformLocation(e->prog, "finc0");
        e->u_finc1 = glGetUniformLocation(e->prog, "finc1");
        e->u_fogc = glGetUniformLocation(e->prog, "fogc");
        e->u_tscale = glGetUniformLocation(e->prog, "tscale");
        e->u_aref = glGetUniformLocation(e->prog, "aref");
        e->u_c = glGetUniformLocation(e->prog, "c");
        e->u_fogp = glGetUniformLocation(e->prog, "fogp");
        e->cgen = ~0u;
        glUseProgram(e->prog);
        for (i = 0; i < 4; i++) {
            snprintf(name, sizeof name, "tex%u", i);
            if ((e->u_tex[i] = glGetUniformLocation(e->prog, name)) >= 0) glUniform1i(e->u_tex[i], (GLint)i);
        }
    }
    e->next = s_vg.programs[h % VG_MAX_PROGRAMS];
    s_vg.programs[h % VG_MAX_PROGRAMS] = e;
    if (++s_vg.nprograms <= 32 || (s_vg.nprograms & 63) == 0)
        fprintf(stderr, "[HW vita] shader de combiners #%u (%u etapas)%s\n", s_vg.nprograms,
                k->control & 0xFF, e->prog ? "" : " FALLÓ: ese estado no se dibuja");
    return e;
}

/* Blit a pantalla (superficie del juego, vídeo): a_pos = (x, y, u, v). */
static void vg_blit_init(void)
{
    static const char vs_src[] =
#if defined(RESOT_HW_GLSL)
        "attribute float4 a_pos; varying float2 v_uv;\n"
        "void main() { v_uv = a_pos.zw; gl_Position = float4(a_pos.xy, 0.0, 1.0); }\n";
#else
        "void main(float4 a_pos, float4 out gl_Position : POSITION, float2 out v_uv : TEXCOORD0) {\n"
        "  v_uv = a_pos.zw; gl_Position = float4(a_pos.xy, 0.0, 1.0); }\n";
#endif
    static const char fs_src[] =
#if defined(RESOT_HW_GLSL)
        "varying float2 v_uv; uniform sampler2D tex;\n"
        "void main() { gl_FragColor = float4(tex2D(tex, v_uv).xyz, 1.0); }\n";
#else
        "float4 main(float2 v_uv : TEXCOORD0, uniform sampler2D tex) : COLOR {\n"
        "  return float4(tex2D(tex, v_uv).xyz, 1.0); }\n";
#endif
    static const char *const attrs[1] = {"a_pos"};
    char *v = (char *)malloc(sizeof s_vg_prelude + sizeof vs_src), *f = (char *)malloc(sizeof s_vg_prelude + sizeof fs_src);
    GLuint vs, fs;
    strcpy(v, s_vg_prelude); strcat(v, vs_src);
    strcpy(f, s_vg_prelude); strcat(f, fs_src);
    vs = vg_shader(VG_VERTEX_SHADER, v);
    fs = vg_shader(VG_FRAGMENT_SHADER, f);
    free(v); free(f);
    if (!vs || !fs || !(s_vg.blit = vg_link(vs, fs, attrs, 1))) vg_fail("blit shader");
    s_vg.blit_tex = glGetUniformLocation(s_vg.blit, "tex");
}

/* Rectángulo en píxeles del framebuffer (origen arriba a la izquierda) con la
 * textura completa: la fila 0 de la textura va arriba. */
static void vg_blit(GLuint tex, float x, float y, float w, float h, float fbw, float fbh)
{
    float x0 = 2.0f * x / fbw - 1.0f, x1 = 2.0f * (x + w) / fbw - 1.0f;
    float y0 = 1.0f - 2.0f * y / fbh, y1 = 1.0f - 2.0f * (y + h) / fbh;   /* pantalla: NDC +1 arriba */
    float q[6][4] = {{x0, y0, 0, 0}, {x1, y0, 1, 0}, {x0, y1, 0, 1},
                     {x1, y0, 1, 0}, {x1, y1, 1, 1}, {x0, y1, 0, 1}};
    int i;
    glUseProgram(s_vg.blit);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(s_vg.blit_tex, 0);
    for (i = 1; i < 8; i++) glDisableVertexAttribArray((GLuint)i);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, q);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* ── init ────────────────────────────────────────────────── */

static int ng_init(const char *backend)
{
    static const uint32_t white[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    const char *flip = getenv("RECOMP_VITA_RT_FLIP");
    int f;
    (void)backend;
    /* 16 MB de RAM quedan para el runtime (pilas de hilos, archivos); vitaGL
     * toma el resto de la memoria libre para su pool. */
    if (!vglInitExtended(0, VG_SCREEN_W, VG_SCREEN_H, 16 * 1024 * 1024, VG_MSAA_NONE)) {
        fprintf(stderr, "[HW vita] vglInitExtended falló: rasterizador por software\n");
        return 0;
    }
    s_vg.ysign = (flip && *flip ? atoi(flip) : VG_RT_FLIP_DEFAULT) ? -1 : 1;
    s_vg.gpu_vsh = !(getenv("RECOMP_VITA_CPU_VSH") && atoi(getenv("RECOMP_VITA_CPU_VSH")));
    vg_blit_init();
    glGenTextures(1, &s_vg.white);
    glBindTexture(GL_TEXTURE_2D, s_vg.white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &s_vg.white_cube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, s_vg.white_cube);
    for (f = 0; f < 6; f++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    /* 25 MB del pool RAM de vitaGL (vglMalloc probaría newlib primero): en el heap de newlib
     * el malloc fallaba y todo caía en silencio al rasterizador por software. */
#if defined(RESOT_HW_GLSL)
    s_hw.verts = (HwVertex *)malloc(sizeof(HwVertex) * HW_MAX_VERTS);
#else
    s_hw.verts = (HwVertex *)vglAlloc(sizeof(HwVertex) * HW_MAX_VERTS, VGL_MEM_RAM);
#endif
    if (!s_hw.verts) {
        fprintf(stderr, "[HW vita] sin memoria para %u vértices: rasterizador por software\n", (unsigned)HW_MAX_VERTS);
        VG_GL_LEAVE();
        return 0;
    }
    s_hw.cpu_vsh = 1;
    s_hw.cull_face = 0x405; s_hw.front_face = 0x900;
    s_vg.active = 1;
    InterlockedExchange(&s_hw_backend, 100);
    xbox_FramebufferWindowHwMode(1);
    fprintf(stderr, "[HW] renderer activo: vitaGL (%s; vertex programs en la %s; escala %d; filas %s)\n",
            (const char *)glGetString(GL_RENDERER), s_vg.gpu_vsh ? "GPU" : "CPU", s_hw.scale, s_vg.ysign < 0 ? "NDC+1" : "NDC-1");
    VG_GL_LEAVE();   /* fuera de s_hw.lock: el resto llega con el lock (nv2a_vita_glthread.h) */
    return 1;
}

/* ── superficies y texturas ──────────────────────────────── */

static void vg_bind_surface(VgSurface *s)
{
    static GLuint bound = ~0u;
    if (bound != s->fbo) { glBindFramebuffer(GL_FRAMEBUFFER, s->fbo); bound = s->fbo; }
}

static VgSurface *ng_surface(void)
{
    uint32_t bpp = surface_bpp(), w, h, off, i;
    VgSurface *out = NULL, *old = &s_vg.surfaces[0];
    int percent = (int)InterlockedCompareExchange(&s_render_percent, 0, 0);
    if (percent != s_vg.applied_percent) {   /* escala nueva: superficies nuevas */
        for (i = 0; i < HW_MAX_SURFACES; i++) {
            if (s_vg.surfaces[i].fbo) glDeleteFramebuffers(1, &s_vg.surfaces[i].fbo);
            if (s_vg.surfaces[i].tex) glDeleteTextures(1, &s_vg.surfaces[i].tex);
        }
        for (i = 0; i < HW_MAX_DEPTHS; i++)
            if (s_vg.depths[i].rb) glDeleteRenderbuffers(1, &s_vg.depths[i].rb);
        memset(s_vg.surfaces, 0, sizeof s_vg.surfaces);
        memset(s_vg.depths, 0, sizeof s_vg.depths);
        s_vg.front = s_vg.largest = NULL;
        s_vg.applied_percent = percent;
    }
    if (!s_gpu.color_offset || !bpp || !s_gpu.pitch) return NULL;
    off = dma_resolve(s_gpu.color_offset); w = s_gpu.pitch / bpp;
    h = s_gpu.clip_y + s_gpu.clip_h;
    if (!w || !h || w > 4096 || h > 4096) return NULL;
    for (i = 0; i < HW_MAX_SURFACES; i++) {
        VgSurface *s = &s_vg.surfaces[i];
        if (s->fbo && s->offset == off && s->w == w) {
            if (s->h >= h) return s;
            out = s; break;
        }
        if (!s->fbo || (old->fbo && s->frame < old->frame)) old = s;
    }
    if (!out) out = old;
    if (out->fbo) { glDeleteFramebuffers(1, &out->fbo); glDeleteTextures(1, &out->tex); }
    if (s_vg.front == out) s_vg.front = NULL;
    if (s_vg.largest == out) s_vg.largest = NULL;
    memset(out, 0, sizeof *out);
    out->offset = off; out->w = w; out->h = h; out->sc = hw_surface_scale(w);
    glGenTextures(1, &out->tex);
    glBindTexture(GL_TEXTURE_2D, out->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)hw_px(w, out->sc), (GLsizei)hw_py(h, out->sc), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &out->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, out->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out->tex, 0);
    vg_bind_surface(out);   /* sincroniza la caché de vg_bind_surface */
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        static uint32_t created;
        if (++created <= 24)
            fprintf(stderr, "[HW vita] superficie 0x%08X %ux%u (host %ux%u)\n", off, w, h,
                    hw_px(w, out->sc), hw_py(h, out->sc));
    }
    return out;
}

static void ng_mark(VgSurface *s)
{
    s->frame = s_hw.frame;
    if (!s_vg.largest || s->w * s->h >= s_vg.largest->w * s_vg.largest->h) s_vg.largest = s;
}

/* El origen 160x120 de la cadena de glow dibujado este frame (hw_postprocess_candidate). */
static int ng_postprocess_source(uint32_t offset)
{
    uint32_t i;
    for (i = 0; i < HW_MAX_SURFACES; i++) {
        const VgSurface *s = &s_vg.surfaces[i];
        if (s->fbo && s->offset == offset && s->w == 160 && s->h == 120 && s->frame == s_hw.frame) return 1;
    }
    return 0;
}

/* Depth por offset de zeta, independiente del color (varios colores comparten uno). */
static GLuint vg_depth(VgSurface *s)
{
    VgDepth *depth = NULL;
    uint32_t i, w = hw_px(s->w, s->sc), h = hw_py(s->h, s->sc);
    static uint32_t next;
    GLuint rb = 0;
    if (s_gpu.zeta_offset) {
        for (i = 0; i < HW_MAX_DEPTHS && !rb; i++) {
            VgDepth *d = &s_vg.depths[i];
            if (d->rb && d->offset == s_gpu.zeta_offset && d->w == w && d->h == h) rb = d->rb;
            else if (!d->rb && !depth) depth = d;
        }
        if (!rb) {
            if (!depth) depth = &s_vg.depths[next++ % HW_MAX_DEPTHS];
            if (depth->rb) glDeleteRenderbuffers(1, &depth->rb);
            glGenRenderbuffers(1, &depth->rb);
            glBindRenderbuffer(GL_RENDERBUFFER, depth->rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)w, (GLsizei)h);
            depth->offset = s_gpu.zeta_offset; depth->w = w; depth->h = h;
            rb = depth->rb;
        }
    }
    if (s->depth != rb) {
        vg_bind_surface(s);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
        if (rb && s->depth == 0) {   /* depth nuevo para esta superficie: limpio */
            glDisable(GL_SCISSOR_TEST);
            glDepthMask(GL_TRUE);
            glClearDepthf(1.0f);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }
        s->depth = rb;
    }
    return rb;
}

static void vg_sampler(VgTexture *c, const Texture *t, GLenum target)
{
    static const GLint addr[6] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT,
                                  GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE};
    uint32_t mag = (t->filter >> 24) & 15, min = (t->filter >> 16) & 255;
    int linear = mag == 2 || (min >= 2 && min <= 6);
    GLint ws = addr[t->addr_u < 6 ? t->addr_u : 3], wt = addr[t->addr_v < 6 ? t->addr_v : 3];
    GLint fmin = c->levels > 1 ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST)
                               : (linear ? GL_LINEAR : GL_NEAREST);
    GLint fmag = linear ? GL_LINEAR : GL_NEAREST;
    if (c->wrap_s != ws) { glTexParameteri(target, GL_TEXTURE_WRAP_S, ws); c->wrap_s = ws; }
    if (c->wrap_t != wt) { glTexParameteri(target, GL_TEXTURE_WRAP_T, wt); c->wrap_t = wt; }
    if (c->min != fmin) { glTexParameteri(target, GL_TEXTURE_MIN_FILTER, fmin); c->min = fmin; }
    if (c->mag != fmag) { glTexParameteri(target, GL_TEXTURE_MAG_FILTER, fmag); c->mag = fmag; }
}

/* La textura de la etapa: un render target por offset, o la decodificada de
 * memoria guest (sample_tex, como el rasterizador por software) en caché. */
static GLuint vg_texture(int stage, VgSurface *target)
{
    const Texture *t = &s_gpu.texs[stage];
    VgSurface *best = NULL;
    VgTexture *c = NULL, *victim = NULL;
    GLenum gtarget = t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    uint32_t i, slot, hash, layers, face, x, y, *px, levels = 1;
    {
        static int logged;
        if (logged++ < 12)
            fprintf(stderr, "[HW vita] textura etapa %d: valid %d 0x%08X %ux%u fmt 0x%X cube %d mem_ok %d\n", stage,
                    t->valid, t->offset, t->width, t->height, t->color, t->cube,
                    hw_mem_ok(t->offset, t->cube ? 6 * tex_face_stride(t) : hw_tex_bytes(t)));
    }
    if (!t->valid || !t->width || !t->height || t->width > 4096 || t->height > 4096) return 0;
    for (i = 0; !t->cube && i < HW_MAX_SURFACES; i++) {
        VgSurface *s = &s_vg.surfaces[i];
        if (s->fbo && s->offset == t->offset && (!best ||
            (s->w == t->width && best->w != t->width) ||
            ((s->w == t->width) == (best->w == t->width) && s->frame > best->frame))) best = s;
    }
    if (best) return best == target ? 0 : best->tex;
    if (!hw_mem_ok(t->offset, t->cube ? 6 * tex_face_stride(t) : hw_tex_bytes(t))) return 0;
    slot = (t->offset >> 6) * 2654435761u >> 22;
    for (i = 0; i < 16; i++) {
        VgTexture *e = &s_vg.textures[(slot + i) & (HW_MAX_TEXTURES - 1)];
        const Texture *g = &e->guest;
        if (e->tex && g->offset == t->offset && g->color == t->color &&
            g->width == t->width && g->height == t->height && g->pitch == t->pitch &&
            g->palette == t->palette && g->cube == t->cube && g->levels == t->levels) { c = e; break; }
        if (!victim || !e->tex || (victim->tex && e->used < victim->used)) victim = e;
    }
    if (c && c->checked == s_hw.frame) { c->used = s_hw.frame; goto bind; }
    hash = hw_tex_hash(t);
    if (c && c->hash == hash) { c->checked = c->used = s_hw.frame; goto bind; }
    if (!c) c = victim;
    if (!c->tex) glGenTextures(1, &c->tex);
    else if (c->guest.cube != t->cube) { glDeleteTextures(1, &c->tex); glGenTextures(1, &c->tex); }
    c->wrap_s = c->wrap_t = c->min = c->mag = -1;
    layers = t->cube ? 6 : 1;
    if (t->levels > 1 && tex_size_from_format(t->color)) {
        uint32_t extent = t->width > t->height ? t->width : t->height;
        while ((extent >>= 1) != 0) levels++;
    }
    px = (uint32_t *)malloc((size_t)t->width * t->height * 4);
    if (!px) vg_fail("texture decode allocation");
    glBindTexture(gtarget, c->tex);
    for (face = 0; face < layers; face++) {
        for (y = 0; y < t->height; y++) for (x = 0; x < t->width; x++) {
            uint32_t argb = ~0u;
            sample_tex(t, t->cube ? face * tex_face_stride(t) : 0, x, y, &argb);
            /* ARGB -> bytes RGBA */
            px[(size_t)y * t->width + x] = (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFF) | ((argb & 0xFF) << 16);
        }
        glTexImage2D(t->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D, 0, GL_RGBA,
                     (GLsizei)t->width, (GLsizei)t->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    free(px);
    if (levels > 1) glGenerateMipmap(gtarget);
    c->guest = *t; c->hash = hash; c->levels = levels; c->checked = c->used = s_hw.frame;
    s_hw.tex_uploads++;
bind:
    glBindTexture(gtarget, c->tex);
    vg_sampler(c, t, gtarget);
    return c->tex;
}

/* ── dibujo ──────────────────────────────────────────────── */

static GLenum vg_factor(uint32_t f)
{
    switch (f) {
    case 0: return GL_ZERO;
    case 1: return GL_ONE;
    case 0x300: case 0x301: case 0x302: case 0x303: case 0x304:
    case 0x305: case 0x306: case 0x307: case 0x308: return (GLenum)f;   /* GL enums, como el NV2A */
    case 0x8001: case 0x8003: return GL_ONE;    /* ponytail: sin color constante en vitaGL */
    case 0x8002: case 0x8004: return GL_ZERO;
    default: return GL_ONE;
    }
}

static void vg_scissor(VgSurface *s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    GLint sx = (GLint)hw_px(x0, s->sc), sw = (GLint)hw_px(x1, s->sc) - sx;
    GLint gy0 = (GLint)hw_py(y0, s->sc), sh = (GLint)hw_py(y1, s->sc) - gy0;
    GLint H = (GLint)hw_py(s->h, s->sc);
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    glEnable(GL_SCISSOR_TEST);
    /* scissor de GL cuenta desde NDC -1: ahí está el guest y=0 si ysign > 0 */
    glScissor(sx, s_vg.ysign > 0 ? gy0 : H - gy0 - sh, sw, sh);
}

static void vg_state(VgSurface *s, GLuint depth)
{
    int blend = s_gpu.blend_enable && !(s_gpu.blend_sfactor == 1 && s_gpu.blend_dfactor == 0);
    int cull = s_hw.cull_enable && !s_hw.nocull;
    uint32_t front = s_hw.front_face;
    glViewport(0, 0, (GLsizei)hw_px(s->w, s->sc), (GLsizei)hw_py(s->h, s->sc));
    vg_scissor(s, s_gpu.clip_x, s_gpu.clip_y, s_gpu.clip_x + s_gpu.clip_w, s_gpu.clip_y + s_gpu.clip_h);
    glColorMask((s_gpu.color_mask & 0x10000) != 0, (s_gpu.color_mask & 0x100) != 0,
                (s_gpu.color_mask & 1) != 0, (s_gpu.color_mask & 0x1000000) != 0);
    if (blend) {
        GLenum src = vg_factor(s_gpu.blend_sfactor), dst = vg_factor(s_gpu.blend_dfactor);
        GLenum op = s_gpu.blend_equation;
        glEnable(GL_BLEND);
        glBlendFuncSeparate(src, dst, src, dst);
        glBlendEquation(op == 0x800A || op == 0x800B || op == 0x8007 || op == 0x8008 ? op : GL_FUNC_ADD);
    } else glDisable(GL_BLEND);
    if (depth && s_gpu.depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(0x200 | (s_gpu.depth_func & 7));
        glDepthMask(s_gpu.depth_mask ? GL_TRUE : GL_FALSE);
    } else glDisable(GL_DEPTH_TEST);
    if (cull && (s_hw.cull_face == 0x404 || s_hw.cull_face == 0x405 || s_hw.cull_face == 0x408)) {
        glEnable(GL_CULL_FACE);
        glCullFace(s_hw.cull_face);
        /* guest y=0 en NDC -1 refleja la imagen respecto de D3D: invierte el sentido */
        if (s_vg.ysign > 0) front = front == 0x900 ? 0x901 : 0x900;
        glFrontFace(front);
    } else glDisable(GL_CULL_FACE);
}

static void vg_constants(VgSurface *s, VgPsKey *k, float vp[4], float ws[4], GLuint tex[4])
{
    int main_view = s->w == 640 && s_gpu.clip_w == 640 && s_gpu.clip_h >= 448;
    uint32_t i, n;
    memset(k, 0, sizeof *k);
    vp[0] = 2.0f / s->w; vp[1] = 2.0f / s->h; vp[2] = 1.0f / hw_zmax_bits(); vp[3] = (float)s_vg.ysign;
    ws[0] = main_view && !s_hw.batch_full_width ? 1.0f / s_hw.widen : 1;
    if (main_view && s_hw.batch_projected_screen) ws[0] = s_hw.widen;
    if (main_view && s_hw.batch_projected_2d) ws[0] = 1;
    ws[1] = (float)s->w; ws[2] = 1; ws[3] = 0;
    if (main_view && s_hw.batch_hud) { ws[0] = 1; ws[3] = hw_hud_shift(); }
    k->rc_seen = (uint32_t)s_gpu.rc_seen;
    if (k->rc_seen) {
        k->control = s_gpu.rc.control; k->sp = s_gpu.rc.stage_program;
        k->final0 = s_gpu.rc.final0; k->final1 = s_gpu.rc.final1;
        k->cpm = s_gpu.clip_plane_mode;
        n = k->control & 0xFF;
        for (i = 0; i < n && i < 8; i++) {
            k->cw[i][0] = s_gpu.rc.color_icw[i]; k->cw[i][1] = s_gpu.rc.alpha_icw[i];
            k->cw[i][2] = s_gpu.rc.color_ocw[i]; k->cw[i][3] = s_gpu.rc.alpha_ocw[i];
        }
    }
    k->atest_func = s_gpu.alpha_test ? (s_gpu.alpha_func ? s_gpu.alpha_func : 0x207) : 0;
    for (i = 0; i < 4; i++) {
        const Texture *t = &s_gpu.texs[i];
        uint32_t mode = (s_gpu.rc.stage_program >> (5 * i)) & 31;
        tex[i] = 0;
        if ((s_gpu.rc_seen ? mode >= 1 && mode <= 3 : i == 0) && t->valid) {
            glActiveTexture(GL_TEXTURE0 + i);
            tex[i] = vg_texture((int)i, s);
        }
        if (tex[i]) { k->bound |= 1u << i; if (t->cube) k->cube |= 1u << i; }
    }
}

/* Superficie, programa, estado, uniforms y texturas de un dibujo del guest.
 * vsp: 0 = vértices ya transformados en la CPU; si no, el vertex program. */
static VgProgram *vg_begin_draw(VgSurface **out, uint32_t vsp)
{
    VgSurface *s = ng_surface();
    VgPsKey key;
    VgProgram *p;
    float vp[4], ws[4], c[8][4], c1[8][4], tmp[4], tscale[4][4];
    GLuint depth, tex[4];
    uint32_t i;
    if (!s) return NULL;
    depth = vg_depth(s);
    vg_constants(s, &key, vp, ws, tex);
    key.vsp = vsp;
    p = vg_program(&key);
    if (!p->prog) return NULL;
    vg_bind_surface(s);
    vg_state(s, depth);
    glUseProgram(p->prog);
    if (p->u_vp >= 0) glUniform4fv(p->u_vp, 1, vp);
    if (p->u_ws >= 0) glUniform4fv(p->u_ws, 1, ws);
    for (i = 0; i < 8; i++) { hw_color4(s_gpu.rc.factor0[i], c[i]); hw_color4(s_gpu.rc.factor1[i], c1[i]); }
    if (p->u_fac0 >= 0) glUniform4fv(p->u_fac0, 8, &c[0][0]);
    if (p->u_fac1 >= 0) glUniform4fv(p->u_fac1, 8, &c1[0][0]);
    if (p->u_finc0 >= 0) { hw_color4(s_gpu.rc.final_c0, tmp); glUniform4fv(p->u_finc0, 1, tmp); }
    if (p->u_finc1 >= 0) { hw_color4(s_gpu.rc.final_c1, tmp); glUniform4fv(p->u_finc1, 1, tmp); }
    if (p->u_fogc >= 0) {
        float r;
        hw_color4(s_gpu.fog_color, tmp);
        r = tmp[2]; tmp[2] = tmp[0]; tmp[0] = r;   /* el registro de fog guarda R en el byte bajo */
        glUniform4fv(p->u_fogc, 1, tmp);
    }
    if (p->u_aref >= 0) glUniform1f(p->u_aref, (float)(s_gpu.alpha_ref & 0xFF));
    for (i = 0; i < 4; i++) {
        const Texture *t = &s_gpu.texs[i];
        tscale[i][0] = tscale[i][1] = 1; tscale[i][2] = tscale[i][3] = 0;
        if (t->valid && !tex_size_from_format(t->color)) { tscale[i][0] = 1.0f / t->width; tscale[i][1] = 1.0f / t->height; }
        glActiveTexture(GL_TEXTURE0 + i);
        if (tex[i]) glBindTexture(t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, tex[i]);
        else glBindTexture(GL_TEXTURE_2D, s_vg.white);
    }
    if (p->u_tscale >= 0) glUniform4fv(p->u_tscale, 4, &tscale[0][0]);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    *out = s;
    return p;
}

static void ng_draw(const HwVertex *v, uint32_t count)
{
    VgSurface *s;
    uint32_t i;
    if (!count || !vg_begin_draw(&s, 0)) return;
    for (i = 0; i < 8; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(HwVertex), &v[0].pos[0] + 4 * i);
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
    if (s_vg.vis_enabled) {   /* área en píxeles guest, sin oclusión (aproximado) */
        for (i = 0; i + 2 < count; i += 3) {
            float ax = v[i + 1].pos[0] - v[i].pos[0], ay = v[i + 1].pos[1] - v[i].pos[1];
            float bx = v[i + 2].pos[0] - v[i].pos[0], by = v[i + 2].pos[1] - v[i].pos[1];
            float a = 0.5f * (ax * by - ay * bx);
            s_vg.vis_pixels += a < 0 ? -a : a;
        }
    }
    ng_mark(s); s_hw.draws++; s_hw.cpu_vsh_batches++; s_hw.tris += count / 3;
    s_gpu.tris_drawn += count / 3; s_gpu.xf_drawn += count / 3;
}

/* ── vertex programs en la GPU (V1.2) ────────────────────────
 * Cada programa del NV2A se traduce una vez con el emisor de upstream
 * (nv2a_vsh_emit_hlsl, el del camino D3D11) y corre en la GPU: aquí solo se
 * leen los atributos crudos (fetch_attr) de los vértices distintos del lote
 * y la primitiva va como índices. En la Vita el intérprete de la CPU sería
 * el cuello de botella (cientos de miles de vértices por cuadro en el menú).
 * RECOMP_VITA_CPU_VSH=1 vuelve al intérprete. */

/* Las funciones de s_hw_hlsl_vsp_head de upstream, en CG/GLSL. */
static const char s_vg_vsp_helpers[] =
    "float vsh_rcp(float x) { return x == 0.0 ? 3.0e38 : 1.0 / x; }\n"
    "float vsh_rcc(float x) {\n"
    "  float f = 1.0 / x;\n"
    "  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
    "  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
    "  return f; }\n"
    "float4 vsh_log(float4 cc) {\n"
    "  float ax = abs(cc.x); float4 r = cc;\n"
    "  if (ax == 0.0) { r.x = -3.0e38; r.z = -3.0e38; r.y = 1.0; }\n"
    "  else { float e = floor(log2(ax)); r.x = e; r.y = ax / exp2(e); r.z = log2(ax); }\n"
    "  r.w = 1.0; return r; }\n"
    "float4 vsh_lit(float4 cc) {\n"
    "  float nl = max(cc.x, 0.0), nh = max(cc.y, 0.0);\n"
    "  float pw = clamp(cc.w, -127.9961, 127.9961);\n"
    "  return float4(1.0, nl, cc.x > 0.0 ? pow(nh, pw) : 0.0, 1.0); }\n"
    "int vsh_cidx(int i) { return (i < 0 || i >= 192) ? 0 : i; }\n"
    /* fog_mode: 0x800..0x804 */
    "float vsh_fog(float d, float4 fogp) {\n"
    "  if (fogp.w < 0.5) return 1.0;\n"
    "  float f;\n"
    "  if (fogp.z == 2048.0 || fogp.z == 2050.0) f = fogp.x + exp2(d * fogp.y * 16.0) - 1.5;\n"
    "  else if (fogp.z == 2049.0 || fogp.z == 2051.0) f = fogp.x + exp2(-d * d * fogp.y * fogp.y * 32.0) - 1.5;\n"
    "  else f = fogp.x + d * fogp.y - 1.0;\n"
    "  if (fogp.z == 2050.0 || fogp.z == 2051.0 || fogp.z == 2052.0) f = abs(f);\n"
    "  if (f != f) f = 1.0;\n"
    "  return saturate(f); }\n";

/* Lo que el HLSL del emisor tiene y CG/GLSL 1.20 no aceptan igual. */
static char *vg_fix_hlsl(char *src)
{
    static const char *const fix[][2] = {
        {").xxxx", ") * float4(1.0, 1.0, 1.0, 1.0)"},
        {"float4 m = 0;", "float4 m = float4(0.0, 0.0, 0.0, 0.0);"},
        {"float4 l = 0;", "float4 l = float4(0.0, 0.0, 0.0, 0.0);"},
        {"a0 = (int)floor(a.x + 0.001);", "a0 = int(floor(a.x + 0.001));"},
    };
    size_t f;
    for (f = 0; f < sizeof fix / sizeof fix[0]; f++) {
        VgBuf b = {0};
        const char *p = src, *q;
        size_t lo = strlen(fix[f][0]);
        while ((q = strstr(p, fix[f][0])) != NULL) {
            vg_emit(&b, "%.*s%s", (int)(q - p), p, fix[f][1]);
            p = q + lo;
        }
        vg_emit(&b, "%s", p);
        free(src);
        src = b.p;
    }
    return src;
}

/* Índice+1 en s_vg.vsp del programa actual traducido, o 0 (intérprete). */
static uint32_t vg_vsp_get(const uint8_t *inputs, uint32_t ninputs)
{
    static uint32_t seen_gen = ~0u, seen;
    uint32_t gen = nv2a_vsh_program_generation(), key, i, n;
    VgVsp *e;
    VgBuf b = {0};
    char *body;
    int ok;
    if (gen == seen_gen) return seen;
    seen_gen = gen; seen = 0;
    key = nv2a_vsh_program_key(&ok);
    if (!ok) return 0;
    for (i = 0; i < s_vg.nvsp; i++) {
        e = &s_vg.vsp[i];
        if (e->key == key && e->ninputs == ninputs && !memcmp(e->inputs, inputs, ninputs))
            return seen = e->vs ? i + 1 : 0;
    }
    if (s_vg.nvsp == VG_MAX_VSP) return 0;
    e = &s_vg.vsp[s_vg.nvsp++];
    memset(e, 0, sizeof *e);
    e->key = key; e->ninputs = ninputs;
    memcpy(e->inputs, inputs, ninputs);
    body = (char *)malloc(1u << 17);
    if (!body) vg_fail("vertex program buffer");
    n = (uint32_t)nv2a_vsh_emit_hlsl(body, 1u << 17);
    if (!n) { free(body); return 0; }
    body = vg_fix_hlsl(body);
    vg_emit(&b, "%s%s", s_vg_prelude, s_vg_vsp_helpers);
#if defined(RESOT_HW_GLSL)
    vg_emit(&b, "attribute float4 a_in0, a_in1, a_in2, a_in3, a_in4, a_in5, a_in6, a_in7;\n"
                "uniform float4 c[192];\nuniform float4 vp, ws, fogp;\n"
                "varying float4 v_d0, v_d1, v_fog, v_t0, v_t1, v_t2, v_t3;\n"
                "void main() {\n");
#else
    vg_emit(&b, "void main(float4 a_in0, float4 a_in1, float4 a_in2, float4 a_in3,\n"
                "  float4 a_in4, float4 a_in5, float4 a_in6, float4 a_in7,\n"
                "  uniform float4 c[192], uniform float4 vp, uniform float4 ws, uniform float4 fogp,\n"
                "  float4 out gl_Position : POSITION, float4 out v_t0 : TEXCOORD0, float4 out v_t1 : TEXCOORD1,\n"
                "  float4 out v_t2 : TEXCOORD2, float4 out v_t3 : TEXCOORD3, float4 out v_d0 : TEXCOORD4,\n"
                "  float4 out v_d1 : TEXCOORD5, float4 out v_fog : TEXCOORD6) {\n");
#endif
    vg_emit(&b, "  float4 v[16]; float4 R[13]; float4 o[13]; int a0 = 0;\n"
                "  for (int k = 0; k < 16; k++) v[k] = float4(0.0, 0.0, 0.0, 0.0);\n"
                "  for (int k = 0; k < 13; k++) { R[k] = float4(0.0, 0.0, 0.0, 0.0); o[k] = R[k]; }\n"
                "  o[3].w = 1.0; o[4].w = 1.0; o[9].w = 1.0; o[10].w = 1.0; o[11].w = 1.0; o[12].w = 1.0;\n");
    for (i = 0; i < ninputs; i++) vg_emit(&b, "  v[%u] = a_in%u;\n", inputs[i], i);
    vg_emit(&b, "%s", body);
    /* vs_finish de upstream con oPos = R[12] (igual que el s_vg_vs fijo) */
    vg_emit(&b, "  float4 P = R[12]; float w = P.w; float x = P.x;\n"
                "  bool flat2d = abs(w - 1.0) < 0.0001;\n"
                "  if (ws.x != 1.0 && flat2d && x > ws.z && x < ws.y - ws.z) x = (x - 0.5 * ws.y) * ws.x + 0.5 * ws.y;\n"
                "  if (flat2d) x += ws.w;\n"
                "  gl_Position = float4((x * vp.x - 1.0) * w, vp.w * (P.y * vp.y - 1.0) * w,\n"
                "                       (2.0 * saturate(P.z * vp.z) - 1.0) * w, w);\n"
                "  v_d0 = saturate(o[3]); v_d1 = saturate(o[4]);\n"
                "  v_fog = float4(vsh_fog(o[5].x, fogp), 0.0, 0.0, 0.0);\n"
                "  v_t0 = o[9]; v_t1 = o[10]; v_t2 = o[11]; v_t3 = o[12];\n}\n");
    free(body);
    e->vs = vg_shader(VG_VERTEX_SHADER, b.p);
    free(b.p);
    if (s_vg.nvsp <= 24 || (s_vg.nvsp & 31) == 0 || !e->vs)
        fprintf(stderr, "[HW vita] vertex program %08X (%u entradas) #%u: %s\n", key, ninputs, s_vg.nvsp,
                e->vs ? "en la GPU" : "NO traducido, intérprete");
    return seen = e->vs ? (uint32_t)(e - s_vg.vsp) + 1 : 0;
}

static int ng_draw_program_batch(void)
{
    static uint32_t stamp[65536], batch_id;
    static uint16_t remap[65536];
    static float (*verts)[8][4];
    static uint16_t *idx;
    const uint32_t *words;
    const uint8_t *inputs;
    uint32_t ninputs, n = s_gpu.idx_count, i, a, nv = 0, ni = 0, vsp;
    VgSurface *s;
    VgProgram *p;
    if (!s_vg.gpu_vsh || n < 3 || n > NV_MAX_INDICES) return 0;
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES: case NV_PRIM_TRIANGLE_STRIP: case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON: case NV_PRIM_QUADS: case NV_PRIM_QUAD_STRIP:
        break;
    default:
        return 1;                           /* puntos y líneas: ningún camino los dibuja */
    }
    if (!nv2a_vsh_pack_gpu(&words, &inputs, &ninputs)) return 0;
    /* La composición del glow (como ng_draw_program_batch de Android). */
    if (hw_postprocess_candidate(n)) {
        Nv2aVshOutput post[4];
        float in[NV2A_VSH_INPUTS][4];
        for (i = 0; i < 4; i++) {
            for (a = 0; a < NV2A_VSH_INPUTS; a++)
                fetch_attr(&s_gpu.attr[a], s_gpu.idx[i], in[a]);
            if (!nv2a_vsh_run((const float (*)[4])in, &post[i])) break;
        }
        if (i == 4 && hw_skip_postprocess(post, 4)) return 1;
    }
    s_hw.batch_full_width = s_hw.batch_projected_screen = s_hw.batch_hud = 0;
    s_hw.batch_projected_2d = 0;   /* ponytail: sin Hor+ (aspect 4:3); con 16:9 hay que clasificar como Android */
    if (!verts) {
        verts = malloc(sizeof *verts * NV_MAX_INDICES);
        idx = malloc((size_t)NV_MAX_INDICES * 6 + 8);
        if (!verts || !idx) vg_fail("program batch buffers");
    }
    EnterCriticalSection(&s_hw.lock);
    if (!(vsp = vg_vsp_get(inputs, ninputs))) { LeaveCriticalSection(&s_hw.lock); return 0; }
    if (++batch_id == 0) { memset(stamp, 0, sizeof stamp); batch_id = 1; }
    for (i = 0; i < n; i++) {
        uint16_t ix = s_gpu.idx[i];
        if (stamp[ix] == batch_id) continue;
        stamp[ix] = batch_id;
        remap[ix] = (uint16_t)nv;
        for (a = 0; a < ninputs; a++) fetch_attr(&s_gpu.attr[inputs[a]], ix, verts[nv][a]);
        for (; a < 8; a++) verts[nv][a][0] = verts[nv][a][1] = verts[nv][a][2] = verts[nv][a][3] = 0.0f;
        nv++;
    }
#define VG_IDX(x, y, z) do { idx[ni] = remap[s_gpu.idx[x]]; idx[ni + 1] = remap[s_gpu.idx[y]]; \
        idx[ni + 2] = remap[s_gpu.idx[z]]; ni += 3; } while (0)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) VG_IDX(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) VG_IDX(i + 1, i, i + 2);      /* conserva el sentido */
            else       VG_IDX(i, i + 1, i + 2);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) VG_IDX(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) { VG_IDX(i, i + 1, i + 2); VG_IDX(i, i + 2, i + 3); }
        break;
    default:                                         /* quad strip */
        for (i = 0; i + 3 < n; i += 2) { VG_IDX(i, i + 1, i + 3); VG_IDX(i, i + 3, i + 2); }
        break;
    }
#undef VG_IDX
    if (ni && (p = vg_begin_draw(&s, vsp)) != NULL) {
        float fogp[4] = { s_gpu.fog_param[0], s_gpu.fog_param[1], (float)s_gpu.fog_mode,
                          s_gpu.fog_enable ? 1.0f : 0.0f };
        uint32_t cgen = nv2a_vsh_constant_generation();
        if (p->u_c >= 0 && p->cgen != cgen) { glUniform4fv(p->u_c, 192, nv2a_vsh_constants()); p->cgen = cgen; }
        if (p->u_fogp >= 0) glUniform4fv(p->u_fogp, 1, fogp);
        for (a = 0; a < 8; a++) {
            glEnableVertexAttribArray(a);
            glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, sizeof verts[0], &verts[0][a][0]);
        }
        glDrawElements(GL_TRIANGLES, (GLsizei)ni, GL_UNSIGNED_SHORT, idx);
        if (s_vg.vis_enabled) s_vg.vis_pixels += ni / 3 * 64.0;   /* ponytail: sin área real en la GPU */
        ng_mark(s); s_hw.draws++; s_hw.gpu_vsh_batches++; s_hw.tris += ni / 3;
        s_gpu.tris_drawn += ni / 3; s_gpu.xf_drawn += ni / 3;
        s_hw_stat_verts += nv;
    }
    LeaveCriticalSection(&s_hw.lock);
    return 1;
}


static void ng_clear(uint32_t param)
{
    VgSurface *s = ng_surface();
    uint32_t x0 = s_gpu.clip_x, y0 = s_gpu.clip_y, x1 = x0 + s_gpu.clip_w, y1 = y0 + s_gpu.clip_h;
    if (!s) return;
    if (s_hw.clear_rect_set) {
        uint32_t cx0 = s_hw.clear_rect_h & 65535, cx1 = (s_hw.clear_rect_h >> 16) + 1;
        uint32_t cy0 = s_hw.clear_rect_v & 65535, cy1 = (s_hw.clear_rect_v >> 16) + 1;
        if (cx0 > x0) x0 = cx0;
        if (cy0 > y0) y0 = cy0;
        if (cx1 < x1) x1 = cx1;
        if (cy1 < y1) y1 = cy1;
    }
    if ((param & 3) && vg_depth(s)) {
        GLbitfield bits = ((param & 1) ? GL_DEPTH_BUFFER_BIT : 0) | ((param & 2) ? GL_STENCIL_BUFFER_BIT : 0);
        vg_bind_surface(s);
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearDepthf((float)(hw_zmax_bits() == 65535 ? s_gpu.zstencil_clear & 65535 : s_gpu.zstencil_clear >> 8) / hw_zmax_bits());
        glClearStencil((GLint)(s_gpu.zstencil_clear & 255));
        glClear(bits);
    }
    if ((param & 0xF0) && x1 > x0 && y1 > y0) {
        float color[4];
        hw_color4(s_gpu.clear_color, color);
        vg_bind_surface(s);
        vg_scissor(s, x0, y0, x1, y1);
        glColorMask((param & 0x10) != 0, (param & 0x20) != 0, (param & 0x40) != 0, (param & 0x80) != 0);
        glClearColor(color[0], color[1], color[2], color[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        ng_mark(s);
    }
    s_gpu.clears++;
}

/* ── pantalla ────────────────────────────────────────────── */

static void ng_compose(void)
{
    const uint32_t *pixels;
    uint32_t w, h, rect[4], serial;
    float pw = VG_SCREEN_H * s_hw.aspect, ph = VG_SCREEN_H, px, py = 0;
    if (pw > VG_SCREEN_W) { pw = VG_SCREEN_W; ph = VG_SCREEN_W / s_hw.aspect; py = (VG_SCREEN_H - ph) * 0.5f; }
    px = (VG_SCREEN_W - pw) * 0.5f;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    vg_bind_surface(&(VgSurface){0});   /* la caché ya no vale */
    glViewport(0, 0, VG_SCREEN_W, VG_SCREEN_H);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (s_vg.front) {
        /* la fila 0 de la superficie es la fila 0 del guest (arriba) en las dos convenciones */
        vg_blit(s_vg.front->tex, px, py, pw, ph, VG_SCREEN_W, VG_SCREEN_H);
    }
    if (xbox_FramebufferOverlayGet(&pixels, &w, &h, rect, &serial) && w && h) {
        float mw = ph * (4.0f / 3.0f), mx = px + (pw - mw) * 0.5f;
        if (!s_vg.overlay || s_vg.overlay_w != w || s_vg.overlay_h != h) {
            if (!s_vg.overlay) glGenTextures(1, &s_vg.overlay);
            glBindTexture(GL_TEXTURE_2D, s_vg.overlay);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            s_vg.overlay_w = w; s_vg.overlay_h = h; s_vg.overlay_serial = serial - 1;
        }
        if (s_vg.overlay_serial != serial) {   /* el vídeo ya viene en bytes RGBA (fb_vita.c) */
            glBindTexture(GL_TEXTURE_2D, s_vg.overlay);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            s_vg.overlay_serial = serial;
        }
        vg_blit(s_vg.overlay, mx + rect[0] * mw / 640, py + rect[1] * ph / 480,
                rect[2] * mw / 640, rect[3] * ph / 480, VG_SCREEN_W, VG_SCREEN_H);
    }
    {   /* diagnóstico cada 5 s: qué se compuso */
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last >= 5000) {
            last = now;
            uint8_t c[4] = {0};
            glReadPixels(VG_SCREEN_W / 2, VG_SCREEN_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
            fprintf(stderr, "[HW vita] compose: centro %02X%02X%02X\n", c[0], c[1], c[2]);
            fprintf(stderr, "[HW vita] compose: front %s %ux%u, vídeo %s, error GL 0x%X\n",
                    s_vg.front ? "sí" : "no", s_vg.front ? s_vg.front->w : 0, s_vg.front ? s_vg.front->h : 0,
                    xbox_FramebufferOverlayGet(&pixels, &w, &h, rect, &serial) ? "sí" : "no", glGetError());
        }
    }
    s_hw.pending_present = 1;
}

static void ng_present(int sync)
{
    (void)sync;   /* vitaGL sincroniza con el vblank */
    if (!s_hw.pending_present) return;
    vglSwapBuffers(GL_FALSE);
    s_hw.pending_present = 0;
    s_hw.last_flip_ms = GetTickCount();
}

static void ng_flip(void)
{
    static DWORD last_log;
    static uint32_t last_frame;
    static uint64_t last_draws;
    DWORD now;
    if (s_hw.pending_present) nv2a_hw_flush_present();
    EnterCriticalSection(&s_hw.lock);
    if (s_vg.largest) s_vg.front = s_vg.largest;
    s_vg.largest = NULL; s_hw.frame++;
    if (s_hw.frame <= 40 || (s_hw.frame & 255) == 0)
        fprintf(stderr, "[HW vita] flip %u: front %ux%u, %llu draws, %llu texturas\n", s_hw.frame,
                s_vg.front ? s_vg.front->w : 0, s_vg.front ? s_vg.front->h : 0,
                (unsigned long long)s_hw.draws, (unsigned long long)s_hw.tex_uploads);
    hw_diag_sample();
    now = GetTickCount();
    if (!last_log) { last_log = now; last_frame = s_hw.frame; last_draws = s_hw.draws; }
    else if (now - last_log >= 5000) {
        uint32_t frames = s_hw.frame - last_frame;
        fprintf(stderr, "[HW vita] %.1f fps; %.1f draws/frame; %u shaders; %llu texturas subidas\n",
                frames * 1000.0 / (now - last_log), frames ? (double)(s_hw.draws - last_draws) / frames : 0,
                s_vg.nprograms, (unsigned long long)s_hw.tex_uploads);
        last_log = now; last_frame = s_hw.frame; last_draws = s_hw.draws;
    }
    ng_compose();
    if (!xbox_FramebufferDeferPresent()) ng_present(1);
    s_hw.last_flip_ms = GetTickCount();
    LeaveCriticalSection(&s_hw.lock);
}

static int ng_readback(uint32_t *dst, uint32_t w, uint32_t h) { (void)dst; (void)w; (void)h; return 0; }

/* ── visibilidad (retail: XBE 0x000CE32E/0x000CE366) ─────── */

static void ng_visibility_poll(void) {}
static void ng_visibility_method(uint32_t method, uint32_t value, uint64_t stamp)
{
    if (method == 0x17C8) s_vg.vis_pixels = 0;
    else if (method == 0x17CC) s_vg.vis_enabled = value != 0;
    else {
        double px = s_vg.vis_pixels;
        /* ponytail: área dibujada sin test de profundidad; GL_SAMPLES_PASSED si
         * un efecto (destellos) se ve a través de paredes. */
        static int logged;
        hw_visibility_complete(NULL, value, stamp, px >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)(px + 0.5));
        if (logged++ < 8)
            fprintf(stderr, "[HW vita] visibilidad: informe en 0x%08X, %.0f píxeles\n", value, px);
    }
}
