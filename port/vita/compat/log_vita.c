/* reSOT Vita: salida única de stdout/stderr hacia logs/log_NNN.log.
 *
 * Antes: stdout y stderr eran dos FILE sobre el mismo archivo y varios hilos
 * escribían trozos de línea, así que el log salía entrelazado ("[THR] pthr[READ]
 * ...") y lleno de repeticiones. Ahora los dos FILE escriben aquí sin búfer;
 * cada hilo junta su línea y se emite entera. Una línea o un bloque de hasta
 * MAXK líneas que se repite exactamente se escribe una vez y luego "  ↑ xN"
 * ("  ↑ K líneas xN" para bloques). vita_log_tick() vuelca la cuenta en curso.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>

#define LMAX 512
#define HIST 16
#define MAXK 8

static SceUID s_fd = -1;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_hist[HIST][LMAX];   /* últimas líneas escritas (anillo) */
static unsigned s_nh;
static char s_cycle[MAXK][LMAX];  /* bloque que se está repitiendo */
static int s_k, s_pos, s_reps;    /* periodo (0: ninguno), posición, repeticiones completas */
static int s_base = 1;            /* 0 si el bloque ya se contó en el aviso anterior */
static int s_after_mark;          /* lo último escrito fue un aviso "↑ xN" */
static __thread char t_line[LMAX];
static __thread int t_len;

/* Como mucho TAG_LIMIT líneas por etiqueta ("[READ]", "[KERNEL]"...) entre dos
 * vita_log_tick() (10 s): el resto se cuenta y se resume en un aviso. */
#define TAGS 32
#define TAG_LIMIT 40
static struct { char tag[24]; unsigned shown, omitted; } s_tags[TAGS];

static int over_limit(const char *line)
{
    const char *a = line, *b;
    size_t n;
    int i;
    while (*a == ' ') a++;
    if (*a != '[' || !(b = strchr(a, ']')) || (n = (size_t)(b - a + 1)) >= sizeof s_tags[0].tag)
        return 0;   /* sin etiqueta: siempre se escribe */
    for (i = 0; i < TAGS; i++) {
        if (!s_tags[i].tag[0]) { memcpy(s_tags[i].tag, a, n); s_tags[i].tag[n] = 0; }
        if (!strncmp(s_tags[i].tag, a, n) && !s_tags[i].tag[n]) break;
    }
    if (i == TAGS) return 0;
    if (s_tags[i].shown < TAG_LIMIT) { s_tags[i].shown++; return 0; }
    s_tags[i].omitted++;
    return 1;
}

static const char *hist_back(unsigned back)   /* 1 = la última */
{
    return s_hist[(s_nh - back) % HIST];
}

static void emit(const char *line)
{
    size_t n = strlen(line);
    if (over_limit(line)) return;
    sceIoWrite(s_fd, line, n);
    sceIoWrite(s_fd, "\n", 1);
    memcpy(s_hist[s_nh % HIST], line, n + 1);
    s_nh++;
    s_after_mark = 0;
}

static void flush_repeat(void)
{
    char mark[64];
    int i, pos = s_pos;
    if (!s_k) return;
    if (s_reps > 0) {
        const char *op = s_base ? "x" : "+";   /* "+N": sigue al aviso anterior */
        int n = s_k == 1 ? snprintf(mark, sizeof mark, "  \xE2\x86\x91 %s%d\n", op, s_reps + s_base)
                         : snprintf(mark, sizeof mark, "  \xE2\x86\x91 %d l\xC3\xADneas %s%d\n", s_k, op, s_reps + s_base);
        sceIoWrite(s_fd, mark, n);
        s_after_mark = 1;
    }
    s_k = 0;
    for (i = 0; i < pos; i++) emit(s_cycle[i]);   /* el trozo de bloque que no llegó a repetirse */
}

static void process(const char *line)
{
    unsigned k;
    int i;
    if (s_k) {
        if (!strcmp(line, s_cycle[s_pos])) {
            if (++s_pos == s_k) { s_pos = 0; s_reps++; }
            return;
        }
        flush_repeat();
    }
    for (k = 1; k <= MAXK && k <= s_nh; k++) {
        if (strcmp(line, hist_back(k))) continue;
        for (i = 0; i < (int)k; i++) memcpy(s_cycle[i], hist_back(k - i), LMAX);
        s_k = (int)k;
        s_base = !s_after_mark;
        s_pos = 1 % s_k;
        s_reps = k == 1;
        return;
    }
    emit(line);
}

static _READ_WRITE_RETURN_TYPE log_write(struct _reent *r, void *cookie, const char *buf,
                                         _READ_WRITE_BUFSIZE_TYPE n)
{
    _READ_WRITE_BUFSIZE_TYPE i;
    (void)r; (void)cookie;
    for (i = 0; i < n; i++) {
        if (buf[i] != '\n' && t_len < LMAX - 1) { t_line[t_len++] = buf[i]; continue; }
        t_line[t_len] = 0;
        pthread_mutex_lock(&s_lock);
        process(t_line);
        pthread_mutex_unlock(&s_lock);
        t_len = 0;
        if (buf[i] != '\n') t_line[t_len++] = buf[i];   /* línea larga: sigue en otra */
    }
    return (_READ_WRITE_RETURN_TYPE)n;
}

/* Tras freopen() de stdout/stderr al archivo: los dos FILE pasan a escribir aquí. */
void vita_log_install(const char *path)
{
    if ((s_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666)) < 0) return;
    fflush(stdout);
    fflush(stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    stdout->_write = log_write;
    stderr->_write = log_write;
}

void vita_log_tick(void)
{
    if (s_fd < 0) return;
    pthread_mutex_lock(&s_lock);
    flush_repeat();
    for (int i = 0; i < TAGS && s_tags[i].tag[0]; i++) {
        if (s_tags[i].omitted) {
            char mark[96];
            int n = snprintf(mark, sizeof mark, "  \xE2\x86\x91 %s +%u omitidas (l\xC3\xADmite %d cada 10 s)\n",
                             s_tags[i].tag, s_tags[i].omitted, TAG_LIMIT);
            sceIoWrite(s_fd, mark, n);
        }
        s_tags[i].shown = s_tags[i].omitted = 0;
    }
    pthread_mutex_unlock(&s_lock);
}
