/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  uni — універсальний режим rofi (плагін)                     │
 * │                                                              │
 * │  • Як combi: вікна, програми, дії, файли (UNI_MODES)         │
 * │  • Символ на початку вибирає режим (працює й на укр. розкл.):│
 * │      (без символу) програми, вікна, швидкі дії               │
 * │      @ команда   → виконати в терміналі     (укр.: ")        │
 * │      / запит     → шукати в інтернеті       (укр.: .)        │
 * │      % дія       → лише швидкі дії                           │
 * │      ' назва     → лише файли домашньої теки (укр.: ' на ~)  │
 * │  • Рядок-підказка завжди каже, що зробить Enter:             │
 * │      22 + 5     → = 27              Enter — копіювати        │
 * │      github.com → відкрити в браузері                        │
 * │      ~/file.pdf → показати у теці                            │
 * │      shutdown now (без збігів) → виконати в терміналі        │
 * │      інший текст (без збігів)  → шукати в інтернеті          │
 * │    (останні два — внизу списку: якщо знайшлась програма,    │
 * │     Enter запускає її, а не команду)                         │
 * │  • Shift+Enter на файлі — показати його у теці               │
 * │  • Пам'ятає останній запит: при відкритті він уже введений   │
 * │    і виділений — почав друкувати / вставив → замінився       │
 * │  • Ctrl+A — виділити все, Ctrl+C — копіювати текст,          │
 * │    Ctrl+V — вставити (вбудовано в rofi)                      │
 * │  • Праворуч — панель «top»: процесор, ядра, відеочип,        │
 * │    пам'ять, мережа (оновлення раз на секунду, лише поки      │
 * │    меню відкрите)                                            │
 * │                                                              │
 * │  Збірка: ~/.config/hypr/src/build.sh                         │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gmodule.h>
#include <pango/pango.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <rofi/mode.h>
#include <rofi/mode-private.h>
#include <rofi/helper.h>
#include <rofi/rofi-icon-fetcher.h>

#include "theme.h"

/* ───── Функції rofi, яких немає в публічних заголовках (але вони експортуються) ───── */
Mode *rofi_collect_modes_search(const char *name);
Mode *script_mode_parse_setup(const char *str);
void *rofi_view_get_active(void);
void  rofi_view_queue_redraw(void);
void  textbox_text(void *tb, const char *text);
void  textbox_cursor_end(void *tb);
void  textbox_set_pango_attributes(void *tb, PangoAttrList *list);
void  widget_queue_redraw(void *wid);
void  rofi_capture_screenshot(void);
void  rofi_view_reload(void);
void  rofi_view_handle_text(void *state, const char *text);
void  rofi_view_trigger_action(void *state, int scope, unsigned int action);
void  rofi_view_maybe_update(void *state);
char *textbox_get_text(const void *tb);
void  widget_set_enabled(void *wid, gboolean enabled);

/* Початок внутрішньої структури RofiViewState (rofi 2.1.1, include/view-internal.h).
 * Використовуємо лише після перевірки, що перше поле — наш режим. */
typedef struct {
    Mode *sw;
    int   refilter;
    void *main_window;
    void *prompt;
    void *text;            /* поле вводу */
    void *case_indicator;
    void *list_view;
    void *overlay;
    void *mesg_box;        /* контейнер повідомлення (панель «top») */
    void *mesg_tb;         /* текст повідомлення */
} ViewHead;

G_MODULE_EXPORT Mode mode;

/* ───────────── Кольори панелі — з теми (~/.config/theme/theme.conf) ───────────── */
#define C_ACCENT TH_ACCENT
#define C_ACC2   TH_ACCENT2
#define C_TEXT   TH_TEXT
#define C_MUTED  TH_MUTED
#define C_LINE   TH_LINE
#define C_GREEN  TH_GREEN
#define C_YELLOW TH_YELLOW
#define C_RED    TH_RED

/* ───────────── Панель «top» ───────────── */
#define UPDATE_MS 2000      /* як часто оновлювати панель (мс). Більше = менше роботи процесору */

/* ───────────── Стан ───────────── */
typedef enum { TOP_NONE, TOP_CALC, TOP_URL, TOP_PATH, TOP_TERM, TOP_WEB } TopKind;
/* Режим за символом на початку поля */
typedef enum { PX_NONE, PX_TERM, PX_WEB, PX_ACTS, PX_FILES } Prefix;

#define MAX_CPU 64
typedef struct {
    /* підрежими */
    Mode        **subs;
    unsigned int *starts, *lengths;
    unsigned int  n_subs, total;
    gboolean     *inited;        /* підрежим уже запущено (ліниво: при відкритті — лише вікна) */
    gboolean      need_reload;   /* додали підрежим — rofi має перечитати кількість рядків */

    /* живий рядок */
    gboolean has_top;        /* рядок-підказка зараз існує (змінюється лише через rofi_view_reload) */
    gboolean top_last;       /* підказка внизу списку (а не вгорі) */
    gboolean want_last;      /* куди її треба поставити після наступного reload */
    Prefix   prefix;         /* режим за символом на початку поля */
    int      acts_sub, files_sub, win_sub;   /* номери підрежимів «acts», «files», «window» (-1 — немає) */
    gboolean empty;          /* шукати нічого (поле порожнє або старий запит ще не активний) → лише відкриті вікна */
    gboolean only_win;       /* список зараз — лише вікна (змінюється лише через rofi_view_reload) */
    gboolean sync_queued;
    TopKind  kind;
    char    *top_text, *top_value;
    gboolean top_is_dir;

    /* введений текст і «виділити все» */
    char    *input;          /* поточний текст поля */
    gboolean sel_active;     /* текст «виділено»: наступне введення замінить його */
    char    *sel_text;
    char    *pending_text;   /* текст, яким треба замінити поле (в idle) */

    /* панель «top» */
    char    *panel;          /* розмітка панелі */
    guint    timer;
    int      ncpu;
    unsigned long long cpu_prev[MAX_CPU + 1][2];   /* [0] = усі ядра; {busy, total} */
    double   cpu_pct[MAX_CPU + 1];
    char    *cpu_name, *gpu_name, *gpu_dir, *temp_path, *net_if;
    char    *bat_dir;        /* /sys/class/power_supply/BATx */
    char    *screen;         /* «eDP-1 · 1920×1080 · 60 Гц» */
    long long rc6_prev; gint64 rc6_time;
    double   gpu_pct;
    long long rx_prev, tx_prev; gint64 net_time;
    double   rx_rate, tx_rate;
} UniPrivate;

static UniPrivate *g_pd = NULL;   /* для таймерів / idle */


/* ═════════════════════════ МАТЕМАТИКА ═════════════════════════
 * Рекурсивний спуск: + - * / ^ ( ), × ÷, десяткова крапка або кома, унарний мінус. */
typedef struct { const char *s; gboolean err; } P;
static void skip(P *p) { while (*p->s == ' ' || *p->s == '\t') p->s++; }
static double expr(P *p);
static double number(P *p) {
    skip(p);
    if (*p->s == '(') { p->s++; double v = expr(p); skip(p); if (*p->s == ')') p->s++; else p->err = TRUE; return v; }
    if (*p->s == '-') { p->s++; return -number(p); }
    if (*p->s == '+') { p->s++; return number(p); }
    char buf[64]; int n = 0; gboolean dot = FALSE;
    while ((isdigit((unsigned char)*p->s) || *p->s == '.' || *p->s == ',') && n < 63) {
        if (*p->s == '.' || *p->s == ',') { if (dot) { p->err = TRUE; break; } dot = TRUE; buf[n++] = '.'; }
        else buf[n++] = *p->s;
        p->s++;
    }
    buf[n] = 0;
    if (n == 0 || (n == 1 && dot)) { p->err = TRUE; return 0; }
    return g_ascii_strtod(buf, NULL);
}
static double power(P *p) { double b = number(p); skip(p); if (*p->s == '^') { p->s++; return pow(b, power(p)); } return b; }
static double term(P *p) {
    double v = power(p);
    for (;;) {
        skip(p);
        if (*p->s == '*') { p->s++; v *= power(p); }
        else if (*p->s == '/') { p->s++; v /= power(p); }
        else if (g_str_has_prefix(p->s, "×")) { p->s += strlen("×"); v *= power(p); }
        else if (g_str_has_prefix(p->s, "÷")) { p->s += strlen("÷"); v /= power(p); }
        else return v;
    }
}
static double expr(P *p) {
    double v = term(p);
    for (;;) { skip(p); if (*p->s == '+') { p->s++; v += term(p); } else if (*p->s == '-') { p->s++; v -= term(p); } else return v; }
}
/* Число для показу: до 12 значущих цифр без «e+12», десяткова кома; group — тисячі через вузький
 * пробіл (1 234 567,89). Без group — для копіювання (1234567,89). */
static char *calc_format(double v, gboolean group) {
    char buf[64];
    double a = fabs(v);
    if (a != 0 && (a >= 1e15 || a < 1e-6)) {               /* дуже велике / мале — лише так */
        g_ascii_formatd(buf, sizeof buf, "%.6g", v);
    } else {
        int intd = a < 1 ? 1 : (int)floor(log10(a)) + 1;   /* цифр до коми */
        char fmt[16]; g_snprintf(fmt, sizeof fmt, "%%.%df", MAX(0, 12 - intd));
        g_ascii_formatd(buf, sizeof buf, fmt, v);
        if (strchr(buf, '.')) {                             /* прибрати зайві нулі в кінці: 2,50 → 2,5 */
            char *e = buf + strlen(buf) - 1;
            while (*e == '0') *e-- = '\0';
            if (*e == '.') *e = '\0';
        }
        if (!strcmp(buf, "-0")) strcpy(buf, "0");
    }
    GString *o = g_string_new(NULL);
    const char *dot = strchr(buf, '.'), *e = strchr(buf, 'e');
    const char *start = buf + (buf[0] == '-');
    const char *int_end = dot ? dot : e ? e : buf + strlen(buf);
    if (buf[0] == '-') g_string_append(o, group ? "−" : "-");  /* у показі — справжній мінус, у копії — звичайний */
    for (const char *c = start; c < int_end; c++) {
        g_string_append_c(o, *c);
        long left = int_end - c - 1;
        if (group && !e && left > 0 && left % 3 == 0) g_string_append(o, "\u202F");   /* вузький пробіл */
    }
    for (const char *c = int_end; *c; c++) {
        if (*c == '.') g_string_append_c(o, ',');
        else g_string_append_c(o, *c);
    }
    return g_string_free(o, FALSE);
}

static char *try_calc(const char *in) {
    gboolean digit = FALSE, op = FALSE;
    for (const char *c = in; *c; c++) {
        if (isdigit((unsigned char)*c)) digit = TRUE;
        else if (strchr("+-*/^", *c) && c != in) op = TRUE;
        else if (g_str_has_prefix(c, "×") || g_str_has_prefix(c, "÷")) { op = TRUE; c++; }
        else if (!strchr(" \t.,()", *c)) return NULL;
    }
    if (!digit || !op) return NULL;
    P p = { in, FALSE };
    double v = expr(&p); skip(&p);
    if (p.err || *p.s != '\0' || isnan(v)) return NULL;
    if (isinf(v)) return g_strdup("∞");
    return calc_format(v, FALSE);
}

/* ═════════════════════════ ПОСИЛАННЯ / ШЛЯХИ ═════════════════════════ */
static GRegex *url_re = NULL;
static char *try_url(const char *in) {
    if (!url_re)
        url_re = g_regex_new("^(https?://\\S+|www\\.\\S+|[\\w-]+(\\.[\\w-]+)*\\.[a-zA-Z]{2,}(:\\d+)?(/\\S*)?|localhost(:\\d+)?(/\\S*)?)$",
                             G_REGEX_OPTIMIZE, 0, NULL);
    if (strchr(in, ' ') || !g_regex_match(url_re, in, 0, NULL)) return NULL;
    if (g_str_has_prefix(in, "http://") || g_str_has_prefix(in, "https://")) return g_strdup(in);
    if (g_str_has_prefix(in, "localhost")) return g_strconcat("http://", in, NULL);
    return g_strconcat("https://", in, NULL);
}
static char *try_path(const char *in, gboolean *is_dir) {
    const char *s = in;
    if (g_str_has_prefix(s, "file://")) s += 7;
    if (!(s[0] == '/' || s[0] == '~')) return NULL;
    char *p = rofi_expand_path(s);
    if (!g_file_test(p, G_FILE_TEST_EXISTS)) { g_free(p); return NULL; }
    *is_dir = g_file_test(p, G_FILE_TEST_IS_DIR);
    return p;
}

/* ═════════════════════════ ЗАПУСК У ФОНІ ═════════════════════════ */
static void run_argv(char **argv) {
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}
static void run_script(const char *a1, const char *a2) {
    char *script = g_build_filename(g_get_home_dir(), ".config/rofi/scripts/universal.py", NULL);
    char *argv[] = { script, (char *)a1, (char *)a2, NULL };
    run_argv(argv);
    g_free(script);
}

/* ═════════════════════════ ДОСТУП ДО ВІКНА ROFI ═════════════════════════ */
static ViewHead *view(void) {
    ViewHead *v = rofi_view_get_active();
    return (v && v->sw == &mode) ? v : NULL;     /* лише якщо це справді наш режим */
}

/* Підсвітити весь текст поля як «виділений» (або зняти підсвічування) */
static void paint_selection(gboolean on) {
    ViewHead *v = view();
    if (!v || !v->text) return;
    if (on) {
        PangoAttrList *l = pango_attr_list_new();
        PangoAttribute *bg = pango_attr_background_new(0x88 * 257, 0xc0 * 257, 0xd0 * 257);   /* акцент */
        PangoAttribute *fg = pango_attr_foreground_new(0x14 * 257, 0x16 * 257, 0x1b * 257);   /* темний текст */
        bg->start_index = fg->start_index = 0;
        bg->end_index = fg->end_index = G_MAXUINT;
        pango_attr_list_insert(l, bg);
        pango_attr_list_insert(l, fg);
        textbox_set_pango_attributes(v->text, l);
        pango_attr_list_unref(l);
    } else {
        textbox_set_pango_attributes(v->text, NULL);
    }
    widget_queue_redraw(v->text);
    rofi_view_queue_redraw();
}

static gboolean idle_paint_selection(gpointer d) {
    if (!view()) return G_SOURCE_CONTINUE;           /* вікно ще не створене — спробуємо пізніше */
    paint_selection(g_pd && g_pd->sel_active);
    return G_SOURCE_REMOVE;
}

/* Замінити текст поля (виконується поза обробкою введення) */
static gboolean idle_replace_text(gpointer d) {
    ViewHead *v = view();
    if (!v || !g_pd || !g_pd->pending_text) return G_SOURCE_REMOVE;
    textbox_text(v->text, g_pd->pending_text);
    textbox_cursor_end(v->text);
    g_clear_pointer(&g_pd->pending_text, g_free);
    widget_queue_redraw(v->text);
    rofi_view_queue_redraw();
    return G_SOURCE_REMOVE;
}

/* Останній запит: ~/.cache/rofi/uni-last.txt */
static void save_last(const char *s) {
    char *dir = g_build_filename(g_get_user_cache_dir(), "rofi", NULL);
    char *p = g_build_filename(dir, "uni-last.txt", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(p, s ? s : "", -1, NULL);
    g_free(dir); g_free(p);
}


/* ═════════════════════════ ПАНЕЛЬ «TOP» ═════════════════════════ */
static char *read_file(const char *path) { char *s = NULL; g_file_get_contents(path, &s, NULL, NULL); return s; }
static long long read_ll(const char *path) { char *s = read_file(path); long long v = s ? g_ascii_strtoll(s, NULL, 10) : -1; g_free(s); return v; }

static const char *level_color(double pct) { return pct < 50 ? C_GREEN : pct < 80 ? C_YELLOW : C_RED; }

/* Горизонтальна смуга заданого кольору */
static void bar_col(GString *o, double pct, int width, const char *color) {
    pct = CLAMP(pct, 0, 100);
    int full = (int)lround(pct / 100.0 * width);
    g_string_append_printf(o, "<span foreground='%s'>", color);
    for (int i = 0; i < full; i++) g_string_append(o, "━");
    g_string_append_printf(o, "</span><span foreground='%s'>", C_LINE);
    for (int i = full; i < width; i++) g_string_append(o, "━");
    g_string_append(o, "</span>");
}
/* Смуга навантаження: зелена → жовта → червона */
static void bar(GString *o, double pct, int width) { bar_col(o, pct, width, level_color(pct)); }

static void human_rate(char *buf, size_t n, double bps) {
    if (bps >= 1024 * 1024) g_snprintf(buf, n, "%.1f МБ/с", bps / 1048576.0);
    else if (bps >= 1024)  g_snprintf(buf, n, "%.0f КБ/с", bps / 1024.0);
    else                   g_snprintf(buf, n, "%.0f Б/с", bps < 0 ? 0 : bps);
}

/* Одноразово: назви процесора / відеочипа, шляхи до сенсорів */
/* Запит до Hyprland напряму через його сокет (без запуску hyprctl). Повертає відповідь або NULL. */
static char *hypr_request(const char *req) {
    const char *sig = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig) return NULL;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    g_snprintf(a.sun_path, sizeof a.sun_path, "%s/hypr/%s/.socket.sock", g_get_user_runtime_dir(), sig);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 || write(fd, req, strlen(req)) < 0) { close(fd); return NULL; }
    GString *o = g_string_new(NULL);
    char buf[4096]; ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) g_string_append_len(o, buf, n);
    close(fd);
    return g_string_free(o, FALSE);
}

/* Кеш незмінних даних (назви процесора й відеочипа): ~/.cache/rofi/sysinfo, два рядки.
   Без кешу щоразу запускався б lspci. Видалити файл — назви перечитаються. */
static char *sysinfo_cache_path(void) { return g_build_filename(g_get_user_cache_dir(), "rofi", "sysinfo", NULL); }

static void sys_init(UniPrivate *pd) {
    char *cache = sysinfo_cache_path(), *cached = read_file(cache);
    if (cached) {
        char **l = g_strsplit(cached, "\n", 3);
        if (l[0] && l[1] && *l[0] && *l[1]) { pd->cpu_name = g_strdup(l[0]); pd->gpu_name = g_strdup(l[1]); }
        g_strfreev(l); g_free(cached);
    }
    gboolean from_cache = pd->cpu_name != NULL;
    /* процесор: «12th Gen Intel(R) Core(TM) i5-1235U» → «Intel Core i5-1235U» */
    char *ci = from_cache ? NULL : read_file("/proc/cpuinfo");
    if (ci) {
        char *m = strstr(ci, "model name");
        if (m && (m = strchr(m, ':'))) {
            m += 2;
            char *e = strchr(m, '\n');
            char *name = g_strndup(m, e ? (gsize)(e - m) : strlen(m));
            GRegex *junk = g_regex_new("\\(R\\)|\\(TM\\)| CPU| Processor|\\s*@.*$|^\\d+th Gen ", 0, 0, NULL);
            char *clean = g_regex_replace_literal(junk, name, -1, 0, "", 0, NULL);
            pd->cpu_name = g_strdup(g_strstrip(clean));
            g_free(clean); g_regex_unref(junk); g_free(name);
        }
        g_free(ci);
    }
    while (pd->ncpu < MAX_CPU) {
        char *p = g_strdup_printf("/sys/devices/system/cpu/cpu%d", pd->ncpu);
        gboolean ok = g_file_test(p, G_FILE_TEST_IS_DIR); g_free(p);
        if (!ok) break;
        pd->ncpu++;
    }
    /* температура: hwmon coretemp / k10temp / zenpower */
    GDir *d = g_dir_open("/sys/class/hwmon", 0, NULL);
    if (d) {
        const char *e;
        while ((e = g_dir_read_name(d)) && !pd->temp_path) {
            char *np = g_strdup_printf("/sys/class/hwmon/%s/name", e);
            char *nm = read_file(np);
            if (nm && (g_str_has_prefix(nm, "coretemp") || g_str_has_prefix(nm, "k10temp") || g_str_has_prefix(nm, "zenpower")))
                pd->temp_path = g_strdup_printf("/sys/class/hwmon/%s/temp1_input", e);
            g_free(nm); g_free(np);
        }
        g_dir_close(d);
    }
    /* відеочип: Intel gt0 (частота, RC6) */
    for (int i = 0; i < 4 && !pd->gpu_dir; i++) {
        char *g = g_strdup_printf("/sys/class/drm/card%d/gt/gt0", i);
        if (g_file_test(g, G_FILE_TEST_IS_DIR)) pd->gpu_dir = g; else g_free(g);
    }
    /* назва відеочипа з lspci: «Alder Lake-UP3 GT2 [Iris Xe Graphics]» → «Iris Xe Graphics» (один раз) */
    for (int i = 0; i < 4 && !pd->gpu_name; i++) {
        char *dev = g_strdup_printf("/sys/class/drm/card%d/device", i);
        char *real = g_file_test(dev, G_FILE_TEST_EXISTS) ? realpath(dev, NULL) : NULL;
        if (real) {
            char *slot = g_path_get_basename(real), *out = NULL;
            char *argv[] = { "lspci", "-mm", "-s", slot, NULL };
            if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL) && out) {
                char *b = strchr(out, '['), *e2 = b ? strchr(b, ']') : NULL;
                if (b && e2) pd->gpu_name = g_strndup(b + 1, e2 - b - 1);
            }
            g_free(out); g_free(slot); free(real);
        }
        g_free(dev);
    }
    if (!pd->gpu_name) pd->gpu_name = g_strdup("GPU");
    else if (!from_cache && pd->cpu_name) {                 /* знайшли обидві назви — запам'ятати */
        char *dir = g_path_get_dirname(cache), *txt = g_strdup_printf("%s\n%s\n", pd->cpu_name, pd->gpu_name);
        g_mkdir_with_parents(dir, 0755);
        g_file_set_contents(cache, txt, -1, NULL);
        g_free(dir); g_free(txt);
    }
    g_free(cache);
    /* батарея: перший power_supply з type = Battery */
    GDir *ps = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (ps) {
        const char *e;
        while ((e = g_dir_read_name(ps)) && !pd->bat_dir) {
            char *tp = g_strdup_printf("/sys/class/power_supply/%s/type", e);
            char *t = read_file(tp);
            if (t && g_str_has_prefix(t, "Battery")) pd->bat_dir = g_strdup_printf("/sys/class/power_supply/%s", e);
            g_free(t); g_free(tp);
        }
        g_dir_close(ps);
    }
    /* екран: назва, роздільність і частота оновлення активного монітора (напряму з сокета Hyprland) */
    {
        char *out = hypr_request("j/monitors");
        if (out) {
            GRegex *re = g_regex_new("\"name\":\\s*\"([^\"]+)\"[^{}]*?\"width\":\\s*(\\d+)[^{}]*?\"height\":\\s*(\\d+)[^{}]*?\"refreshRate\":\\s*([0-9.]+)",
                                     G_REGEX_DOTALL, 0, NULL);
            GMatchInfo *mi = NULL;
            if (re && g_regex_match(re, out, 0, &mi)) {
                char *n = g_match_info_fetch(mi, 1), *w = g_match_info_fetch(mi, 2), *h = g_match_info_fetch(mi, 3), *r = g_match_info_fetch(mi, 4);
                pd->screen = g_strdup_printf("%s · %s×%s · %.0f Гц", n, w, h, g_ascii_strtod(r, NULL));
                g_free(n); g_free(w); g_free(h); g_free(r);
            }
            if (mi) g_match_info_free(mi);
            if (re) g_regex_unref(re);
        }
        g_free(out);
    }
    /* мережа: інтерфейс маршруту за замовчуванням */
    char *rt = read_file("/proc/net/route");
    if (rt) {
        char **lines = g_strsplit(rt, "\n", -1);
        for (int i = 1; lines[i] && !pd->net_if; i++) {
            char ifn[32], dst[16];
            if (sscanf(lines[i], "%31s %15s", ifn, dst) == 2 && strcmp(dst, "00000000") == 0) pd->net_if = g_strdup(ifn);
        }
        g_strfreev(lines); g_free(rt);
    }
}

/* Зчитати лічильники; обчислити відсотки / швидкості відносно попереднього виклику */
static void sys_sample(UniPrivate *pd) {
    char *st = read_file("/proc/stat");
    if (st) {
        char **lines = g_strsplit(st, "\n", -1);
        for (int i = 0; lines[i] && g_str_has_prefix(lines[i], "cpu"); i++) {
            const char *s = lines[i] + 3;
            int idx = 0;
            if (*s != ' ') { idx = atoi(s) + 1; s += strspn(s, "0123456789"); }
            if (idx > MAX_CPU) continue;
            unsigned long long v[8] = {0};
            sscanf(s, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
            unsigned long long idle = v[3] + v[4], total = 0;
            for (int k = 0; k < 8; k++) total += v[k];
            unsigned long long busy = total - idle;
            if (pd->cpu_prev[idx][1] && total > pd->cpu_prev[idx][1])
                pd->cpu_pct[idx] = 100.0 * (busy - pd->cpu_prev[idx][0]) / (total - pd->cpu_prev[idx][1]);
            pd->cpu_prev[idx][0] = busy; pd->cpu_prev[idx][1] = total;
        }
        g_strfreev(lines); g_free(st);
    }
    gint64 now = g_get_monotonic_time();
    /* GPU: зайнятість = 1 − частка часу в RC6 (стан енергозбереження) */
    if (pd->gpu_dir) {
        char *p = g_build_filename(pd->gpu_dir, "rc6_residency_ms", NULL);
        long long rc6 = read_ll(p); g_free(p);
        if (rc6 >= 0 && pd->rc6_time) {
            double el = (now - pd->rc6_time) / 1000.0;
            if (el > 0) pd->gpu_pct = CLAMP(100.0 * (1.0 - (rc6 - pd->rc6_prev) / el), 0, 100);
        }
        pd->rc6_prev = rc6; pd->rc6_time = now;
    }
    /* Мережа: байти з /sys/class/net/<if>/statistics */
    if (pd->net_if) {
        char *rp = g_strdup_printf("/sys/class/net/%s/statistics/rx_bytes", pd->net_if);
        char *tp = g_strdup_printf("/sys/class/net/%s/statistics/tx_bytes", pd->net_if);
        long long rx = read_ll(rp), tx = read_ll(tp);
        if (pd->net_time && rx >= 0) {
            double el = (now - pd->net_time) / 1e6;
            if (el > 0) { pd->rx_rate = (rx - pd->rx_prev) / el; pd->tx_rate = (tx - pd->tx_prev) / el; }
        }
        pd->rx_prev = rx; pd->tx_prev = tx; pd->net_time = now;
        g_free(rp); g_free(tp);
    }
}

#define BARW  44    /* ширина великих смуг (символів) */
#define CBARW 4     /* ширина міні-смуг ядер */
#define CCOLS 4     /* ядер у рядку */

static void section(GString *o, const char *icon, const char *title, const char *right) {
    g_string_append_printf(o, "<span foreground='%s'>%s</span>  <span foreground='%s' weight='bold'>%s</span>", C_ACCENT, icon, C_TEXT, title);
    if (right && *right) g_string_append_printf(o, "  <span foreground='%s' size='small'>%s</span>", C_MUTED, right);
    g_string_append(o, "\n");
}
static void pct_after_bar(GString *o, double pct) {
    g_string_append_printf(o, " <span foreground='%s' weight='bold'>%4.0f%%</span>\n", C_TEXT, pct);
}

/* Побудувати розмітку панелі */
static void sys_render(UniPrivate *pd) {
    GString *o = g_string_new(NULL);
    char buf[64];

    /* ── ПРОЦЕСОР ── */
    double freq = 0; int nf = 0;
    for (int i = 0; i < pd->ncpu; i++) {
        char *p = g_strdup_printf("/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        long long f = read_ll(p); g_free(p);
        if (f > 0) { freq += f; nf++; }
    }
    long long temp = pd->temp_path ? read_ll(pd->temp_path) / 1000 : -1;
    section(o, "", "Процесор", pd->cpu_name);
    bar(o, pd->cpu_pct[0], BARW);
    pct_after_bar(o, pd->cpu_pct[0]);
    g_string_append_printf(o, "<span foreground='%s' size='small'>", C_MUTED);
    if (nf) g_string_append_printf(o, "%.2f ГГц  ·  ", freq / nf / 1e6);
    g_string_append_printf(o, "%d потоків</span>", pd->ncpu);
    if (temp > 0)
        g_string_append_printf(o, "<span size='small'>  ·  </span><span foreground='%s' size='small'>%lld °C</span>",
                               temp >= 85 ? C_RED : temp >= 70 ? C_YELLOW : C_GREEN, temp);
    g_string_append(o, "\n\n");

    /* ── ЯДРА: сітка міні-смуг ── */
    section(o, "\uf2db", "Ядра", NULL);
    for (int i = 0; i < pd->ncpu; i++) {
        double c = pd->cpu_pct[i + 1];
        g_string_append_printf(o, "<span foreground='%s'>%2d</span> ", C_MUTED, i);
        bar(o, c, CBARW);
        g_string_append_printf(o, "<span foreground='%s'>%4.0f%%</span>", C_TEXT, c);
        g_string_append(o, (i % CCOLS == CCOLS - 1 || i == pd->ncpu - 1) ? "\n" : "  ");
    }
    g_string_append(o, "\n");

    /* ── ВІДЕОЧИП ── */
    section(o, "\U000f0379", "Відеочип", pd->gpu_name);
    bar(o, pd->gpu_pct, BARW);
    pct_after_bar(o, pd->gpu_pct);
    if (pd->gpu_dir) {
        char *a = g_build_filename(pd->gpu_dir, "rps_act_freq_mhz", NULL), *m = g_build_filename(pd->gpu_dir, "rps_max_freq_mhz", NULL);
        long long act = read_ll(a), max = read_ll(m); g_free(a); g_free(m);
        g_string_append_printf(o, "<span foreground='%s' size='small'>%lld / %lld МГц", C_MUTED, act < 0 ? 0 : act, max);
        if (pd->screen) g_string_append_printf(o, "  ·  %s", pd->screen);
        g_string_append(o, "</span>\n");
    } else if (pd->screen) g_string_append_printf(o, "<span foreground='%s' size='small'>%s</span>\n", C_MUTED, pd->screen);
    else g_string_append(o, "\n");
    g_string_append(o, "\n");

    /* ── ПАМ'ЯТЬ ── */
    long long mt = 0, ma = 0, swt = 0, swf = 0;
    char *mi = read_file("/proc/meminfo");
    if (mi) {
        char *p;
        if ((p = strstr(mi, "MemTotal:")))     mt  = g_ascii_strtoll(p + 9, NULL, 10);
        if ((p = strstr(mi, "MemAvailable:"))) ma  = g_ascii_strtoll(p + 13, NULL, 10);
        if ((p = strstr(mi, "SwapTotal:")))    swt = g_ascii_strtoll(p + 10, NULL, 10);
        if ((p = strstr(mi, "SwapFree:")))     swf = g_ascii_strtoll(p + 9, NULL, 10);
        g_free(mi);
    }
    double mp = mt ? 100.0 * (mt - ma) / mt : 0;
    section(o, "", "Пам'ять", NULL);
    bar(o, mp, BARW);
    pct_after_bar(o, mp);
    g_string_append_printf(o, "<span foreground='%s' size='small'>%.1f / %.1f ГБ", C_MUTED, (mt - ma) / 1048576.0, mt / 1048576.0);
    if (swt) g_string_append_printf(o, "  ·  swap %.1f / %.1f ГБ", (swt - swf) / 1048576.0, swt / 1048576.0);
    g_string_append(o, "</span>\n\n");

    /* ── БАТАРЕЯ ── */
    if (pd->bat_dir) {
        char *cp = g_build_filename(pd->bat_dir, "capacity", NULL), *sp = g_build_filename(pd->bat_dir, "status", NULL);
        char *pp = g_build_filename(pd->bat_dir, "power_now", NULL), *ep = g_build_filename(pd->bat_dir, "energy_now", NULL);
        char *fp = g_build_filename(pd->bat_dir, "energy_full", NULL);
        long long cap = read_ll(cp), pw = read_ll(pp), en = read_ll(ep), ef = read_ll(fp);
        char *stt = read_file(sp); if (stt) g_strstrip(stt);
        const char *st_ua = !stt ? "" :
            g_str_equal(stt, "Charging")     ? "заряджається" :
            g_str_equal(stt, "Discharging")  ? "від батареї"  :
            g_str_equal(stt, "Full")         ? "заряджена"    :
            g_str_equal(stt, "Not charging") ? "не заряджається" : stt;
        GString *info = g_string_new(st_ua);
        if (pw > 0) {
            g_string_append_printf(info, "  ·  %.1f Вт", pw / 1e6);
            double hours = -1;
            if (stt && g_str_equal(stt, "Discharging") && en > 0) hours = (double)en / pw;
            if (stt && g_str_equal(stt, "Charging") && ef > en)   hours = (double)(ef - en) / pw;
            if (hours > 0) g_string_append_printf(info, "  ·  ≈ %d год %02d хв", (int)hours, (int)((hours - (int)hours) * 60));
        }
        section(o, "\U000f0079", "Батарея", info->str);
        double c = cap < 0 ? 0 : cap;
        bar_col(o, c, BARW, c > 50 ? C_GREEN : c > 20 ? C_YELLOW : C_RED);   /* мало заряду = червоний */
        pct_after_bar(o, c);
        g_string_append(o, "\n");
        g_string_free(info, TRUE); g_free(stt);
        g_free(cp); g_free(sp); g_free(pp); g_free(ep); g_free(fp);
    }

    /* ── МЕРЕЖА ── */
    char sig[48] = "";
    if (pd->net_if) {
        char *w = read_file("/proc/net/wireless");
        char *l = w ? strstr(w, pd->net_if) : NULL;
        float q;
        if (l && (l = strchr(l, ':')) && sscanf(l + 1, "%*s %f", &q) == 1)
            g_snprintf(sig, sizeof sig, "  ·  сигнал %d%%", (int)CLAMP(q * 100 / 70, 0, 100));
        g_free(w);
    }
    char head[96];
    g_snprintf(head, sizeof head, "%s%s", pd->net_if ? pd->net_if : "немає з'єднання", sig);
    section(o, "\U000f05a9", "Мережа", head);
    human_rate(buf, sizeof buf, pd->rx_rate);
    g_string_append_printf(o, "<span foreground='%s'></span> <span foreground='%s' weight='bold'>%s</span>", C_GREEN, C_TEXT, buf);
    human_rate(buf, sizeof buf, pd->tx_rate);
    g_string_append_printf(o, "      <span foreground='%s'></span> <span foreground='%s' weight='bold'>%s</span>", C_ACC2, C_TEXT, buf);

    g_free(pd->panel);
    pd->panel = g_string_free(o, FALSE);
}

/* Таймер: оновити панель (працює лише поки меню відкрите — плагін живе разом з rofi) */
static gboolean sys_tick(gpointer d) {
    UniPrivate *pd = g_pd;
    if (!pd) return G_SOURCE_REMOVE;
    sys_sample(pd);
    sys_render(pd);
    ViewHead *v = view();
    if (g_getenv("UNI_DEBUG")) g_printerr("uni tick: view=%p mesg_box=%p mesg_tb=%p list=%p\n", (void*)v, v ? v->mesg_box : NULL, v ? v->mesg_tb : NULL, v ? v->list_view : NULL);
    if (v && v->mesg_tb) {
        widget_set_enabled(v->mesg_box, TRUE);   /* rofi вмикає блок лише при перезавантаженні — вмикаємо самі */
        textbox_text(v->mesg_tb, pd->panel);
        widget_queue_redraw(v->mesg_box);
        rofi_view_queue_redraw();
    }
    if (pd->timer == 0) {                          /* перший швидкий тик → далі раз на секунду */
        pd->timer = g_timeout_add(UPDATE_MS, sys_tick, NULL);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Тестовий режим: UNI_TEST="type:8;key:41" — імітація введення / клавіш (як із клавіатури) */
static char **test_steps = NULL;
static int test_i = 0;
static gboolean test_step(gpointer d) {
    ViewHead *v = view();
    if (!test_steps || !test_steps[test_i]) return G_SOURCE_REMOVE;
    if (!v) return G_SOURCE_CONTINUE;
    const char *st = test_steps[test_i++];
    if (g_str_has_prefix(st, "type:")) rofi_view_handle_text(v, st + 5);
    else if (g_str_has_prefix(st, "key:")) rofi_view_trigger_action(v, 0 /* SCOPE_GLOBAL */, (unsigned)atoi(st + 4));
    rofi_view_maybe_update(v);              /* як після справжнього натискання: обробити результат / фільтр */
    return test_steps[test_i] ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* Тестовий режим: UNI_SHOT=1 → зберегти знімок вікна й закрити (для перевірки вигляду) */
static gboolean debug_shot(gpointer d) { rofi_capture_screenshot(); exit(0); return G_SOURCE_REMOVE; }


/* ═════════════════════════ ЖИВИЙ РЯДОК ═════════════════════════ */
/* Привести наявність рядка 0 у відповідність до введеного тексту (поза фільтрацією) */
static gboolean idle_sync_top(gpointer d) {
    UniPrivate *pd = g_pd;
    if (!pd) return G_SOURCE_REMOVE;
    pd->sync_queued = FALSE;
    ViewHead *v = view();
    char *txt = (v && v->text) ? textbox_get_text(v->text) : g_strdup(pd->input ? pd->input : "");
    gboolean want = *txt != '\0' && pd->kind != TOP_NONE;
    gboolean only = *txt == '\0' || pd->empty;            /* порожньо / старий запит неактивний — лише вікна */
    g_free(txt);
    if (want != pd->has_top || (want && pd->want_last != pd->top_last) || only != pd->only_win || pd->need_reload) {
        pd->need_reload = FALSE;
        pd->has_top = want;
        pd->top_last = pd->want_last;
        pd->only_win = only;
        rofi_view_reload();                 /* rofi перечитає кількість рядків і перефільтрує */
    }
    return G_SOURCE_REMOVE;
}
static void queue_sync_top(UniPrivate *pd) {
    if (!pd->sync_queued) { pd->sync_queued = TRUE; g_idle_add(idle_sync_top, NULL); }
}

/* Символ на початку → режим. Повертає довжину символу в байтах (0 — символу немає).
 * Українська розкладка: Shift+2 дає «"», клавіша «/» дає «.», клавіша «`» дає «'» або «’». */
static int detect_prefix(const char *in, Prefix *px) {
    static const struct { const char *s; Prefix p; } tbl[] = {
        { "@", PX_TERM }, { "\"", PX_TERM },
        { "/", PX_WEB },  { ".", PX_WEB },
        { "%", PX_ACTS },
        { "'", PX_FILES }, { "\u2019", PX_FILES }, { "\u02bc", PX_FILES }, { "`", PX_FILES },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(tbl); i++)
        if (g_str_has_prefix(in, tbl[i].s)) { *px = tbl[i].p; return (int)strlen(tbl[i].s); }
    *px = PX_NONE;
    return 0;
}

/* Чи перше слово — команда, яку можна запустити */
static gboolean is_command(const char *in) {
    char *first = g_strndup(in, strcspn(in, " \t"));
    char *exp = rofi_expand_path(first);
    char *found = g_find_program_in_path(exp);
    gboolean ok = found != NULL;
    g_free(first); g_free(exp); g_free(found);
    return ok;
}

/* in — текст поля без символу режиму */
static void update_top(UniPrivate *pd, const char *input) {
    g_clear_pointer(&pd->top_text, g_free);
    g_clear_pointer(&pd->top_value, g_free);
    pd->kind = TOP_NONE;
    pd->want_last = FALSE;
    char *in = g_strstrip(g_strdup(input ? input : ""));
    char *v; gboolean is_dir = FALSE;

    if (pd->prefix == PX_TERM) {
        pd->kind = TOP_TERM; pd->top_value = g_strdup(in);
        pd->top_text = *in ? g_strdup_printf("Виконати в терміналі  ·  %s", in)
                           : g_strdup("Термінал  ·  надрукуй команду після @");
    } else if (pd->prefix == PX_WEB) {
        pd->kind = TOP_WEB; pd->top_value = g_strdup(in);
        pd->top_text = *in ? g_strdup_printf("Шукати в інтернеті  ·  %s", in)
                           : g_strdup("Інтернет  ·  надрукуй запит після /");
    } else if (pd->prefix == PX_NONE && *in) {
        if ((v = try_calc(in))) {
            pd->kind = TOP_CALC; pd->top_value = v;                     /* копіюється без пробілів */
            char *shown = g_strdup(v);
            P p = { in, FALSE }; double num = expr(&p);            /* показ — з тисячами */
            if (!p.err && !isinf(num)) { g_free(shown); shown = calc_format(num, TRUE); }
            pd->top_text = g_strdup_printf("= %s", shown);
            g_free(shown);
        } else if ((v = try_path(in, &is_dir))) {
            pd->kind = TOP_PATH; pd->top_value = v; pd->top_is_dir = is_dir;
            pd->top_text = g_strdup_printf(is_dir ? "Відкрити теку  ·  %s" : "Показати у теці  ·  %s", in);
        } else if ((v = try_url(in))) {
            pd->kind = TOP_URL; pd->top_value = v;
            pd->top_text = g_strdup_printf("Відкрити в браузері  ·  %s", v);
        } else if (is_command(in)) {                 /* команда → внизу, під програмами */
            pd->kind = TOP_TERM; pd->top_value = g_strdup(in); pd->want_last = TRUE;
            pd->top_text = g_strdup_printf("Виконати в терміналі  ·  %s", in);
        } else {                                     /* інше → пошук, теж унизу */
            pd->kind = TOP_WEB; pd->top_value = g_strdup(in); pd->want_last = TRUE;
            pd->top_text = g_strdup_printf("Шукати в інтернеті  ·  %s", in);
        }
    }
    g_free(in);
}

static void run_top(UniPrivate *pd) {
    switch (pd->kind) {
    case TOP_CALC: run_script("copy", pd->top_value); break;                 /* копіювати (після закриття rofi) */
    case TOP_URL:  { char *argv[] = { "xdg-open", pd->top_value, NULL }; run_argv(argv); break; }
    case TOP_PATH: run_script("reveal", pd->top_value); break;
    case TOP_TERM: run_script("term", pd->top_value); break;
    case TOP_WEB:  run_script("web", pd->top_value); break;
    default: break;
    }
}


/* ═════════════════════════ ЛІНИВІ ПІДРЕЖИМИ ═════════════════════════ */
/* Запустити один підрежим (якщо ще не запущено). Не вдалося — лишається порожнім (0 рядків). */
static gboolean start_sub(UniPrivate *pd, int i) {
    if (i < 0 || pd->inited[i]) return FALSE;
    pd->inited[i] = TRUE;
    if (!mode_init(pd->subs[i])) { g_warning("uni: не вдалося запустити '%s'", mode_get_name(pd->subs[i])); return FALSE; }
    return TRUE;
}
/* Перерахувати, які рядки якому підрежиму належать */
static void recount(UniPrivate *pd) {
    pd->total = 0;                          /* без живого рядка; він додається через has_top */
    for (unsigned int i = 0; i < pd->n_subs; i++) {
        pd->starts[i] = pd->total;
        pd->lengths[i] = pd->inited[i] ? mode_get_num_entries(pd->subs[i]) : 0;
        pd->total += pd->lengths[i];
    }
}
/* Запустити ще не запущені підрежими (файли — лише якщо with_files). TRUE — щось додалось. */
static gboolean ensure_subs(UniPrivate *pd, gboolean with_files) {
    gboolean changed = FALSE;
    for (unsigned int i = 0; i < pd->n_subs; i++) {
        if ((int)i == pd->files_sub && !with_files) continue;
        if (start_sub(pd, (int)i)) changed = TRUE;
    }
    if (changed) { recount(pd); pd->need_reload = TRUE; }
    return changed;
}


/* ═════════════════════════ ІНТЕРФЕЙС РЕЖИМУ ═════════════════════════ */
static int uni_init(Mode *sw) {
    if (g_getenv("UNI_DEBUG")) g_printerr("uni init (pd=%p)\n", mode_get_private_data(sw));
    if (mode_get_private_data(sw) != NULL) return TRUE;
    UniPrivate *pd = g_malloc0(sizeof *pd);
    mode_set_private_data(sw, pd);
    g_pd = pd;
    pd->acts_sub = pd->files_sub = pd->win_sub = -1;
    pd->only_win = pd->empty = TRUE;         /* відкрили меню — спершу лише вікна */

    const char *spec = g_getenv("UNI_MODES");
    if (!spec || !*spec) spec = "window,drun";
    char **parts = g_strsplit(spec, ",", -1);
    unsigned int n = g_strv_length(parts);
    pd->subs = g_new0(Mode *, n); pd->starts = g_new0(unsigned int, n); pd->lengths = g_new0(unsigned int, n);
    pd->inited = g_new0(gboolean, n);
    /* Підрежими лише СТВОРЮЄМО; запускаємо (mode_init) ліниво — див. ensure_subs():
     * при відкритті потрібні тільки вікна, програми й дії — з першою літерою, файли — лише після «'».
     * Так відкриття меню не запускає python-режими й не читає кеш програм (замір 2026-11-15). */
    for (unsigned int i = 0; i < n; i++) {
        char *t = g_strstrip(parts[i]);
        if (!*t) continue;
        Mode *m = strchr(t, ':') ? script_mode_parse_setup(t) : rofi_collect_modes_search(t);
        if (!m) { g_warning("uni: невідомий режим '%s'", t); continue; }
        const char *nm = mode_get_name(m);
        if (g_strcmp0(nm, "acts") == 0)  pd->acts_sub = (int)pd->n_subs;
        if (g_strcmp0(nm, "files") == 0) pd->files_sub = (int)pd->n_subs;
        if (g_strcmp0(nm, "window") == 0) pd->win_sub = (int)pd->n_subs;
        pd->subs[pd->n_subs++] = m;
    }
    g_strfreev(parts);
    if (pd->win_sub >= 0) start_sub(pd, pd->win_sub);
    else ensure_subs(pd, FALSE);            /* немає вікон у списку — запускаємо все одразу, як раніше */
    recount(pd);

    /* панель «top» */
    sys_init(pd);
    sys_sample(pd);                         /* базові лічильники */
    sys_render(pd);
    g_timeout_add(250, sys_tick, NULL);     /* перші реальні цифри — через 250 мс */

    if (g_getenv("UNI_TEST")) { test_steps = g_strsplit(g_getenv("UNI_TEST"), ";", -1); g_timeout_add(300, test_step, NULL); }
    if (g_getenv("UNI_SHOT")) g_timeout_add(1500, debug_shot, NULL);
    return TRUE;
}

static unsigned int uni_num(const Mode *sw) {
    UniPrivate *pd = mode_get_private_data(sw);
    if (!pd) return 0;
    if (pd->only_win && pd->win_sub >= 0) return pd->lengths[pd->win_sub];   /* нічого не шукаємо — лише відкриті вікна */
    return pd->total + (pd->has_top ? 1 : 0);
}

/* Рядок 0 — живий (якщо has_top), далі — рядки підрежимів */
static gboolean is_top(const UniPrivate *pd, unsigned int line) {
    if (pd->only_win && pd->win_sub >= 0) return FALSE;
    return pd->has_top && line == (pd->top_last ? pd->total : 0);
}

static int find_sub(const UniPrivate *pd, unsigned int line, unsigned int *local) {
    if (pd->only_win && pd->win_sub >= 0) { *local = line; return pd->win_sub; }
    if (pd->has_top && !pd->top_last) line -= 1;
    for (unsigned int i = 0; i < pd->n_subs; i++)
        if (line >= pd->starts[i] && line < pd->starts[i] + pd->lengths[i]) { *local = line - pd->starts[i]; return (int)i; }
    return -1;
}

/* Клавіші kb-custom-N (див. rofi/config.rasi) */
#define KEY_SELECT_ALL 0     /* kb-custom-1: Ctrl+A */
#define KEY_COPY       1     /* kb-custom-2: Ctrl+C */

static ModeMode uni_result(Mode *sw, int mretv, char **input, unsigned int line) {
    UniPrivate *pd = mode_get_private_data(sw);
    const char *in = (input && *input) ? *input : "";

    if (mretv & MENU_CUSTOM_COMMAND) {
        int key = mretv & MENU_LOWER_MASK;
        if (g_getenv("UNI_DEBUG")) g_printerr("uni custom key %d input='%s'\n", key, in);
        if (key == KEY_SELECT_ALL && *in) {                 /* виділити все */
            pd->sel_active = TRUE;
            g_free(pd->sel_text); pd->sel_text = g_strdup(in);
        } else if (key == KEY_COPY && *in) {                /* копіювати текст поля */
            char *argv[] = { "wl-copy", "--", (char *)in, NULL };
            run_argv(argv);
        }
        g_idle_add(idle_paint_selection, NULL);             /* відновити підсвічування після перемальовування */
        return RELOAD_DIALOG;                               /* меню лишається відкритим */
    }

    save_last(in);                                          /* закрили без виконання — запит лишається */

    if (mretv & MENU_NEXT)         return NEXT_DIALOG;
    if (mretv & MENU_PREVIOUS)     return PREVIOUS_DIALOG;
    if (mretv & MENU_QUICK_SWITCH) return (ModeMode)(mretv & MENU_LOWER_MASK);
    if (mretv & MENU_CANCEL)       return MODE_EXIT;

    if (mretv & MENU_CUSTOM_INPUT) {                        /* текст без збігів / Ctrl+Enter */
        if (pd->kind != TOP_NONE) { run_top(pd); save_last(""); }
        else if (*in && pd->prefix == PX_NONE) { run_script("text", in); save_last(""); }
        return MODE_EXIT;                                   /* у режимах % і ' без збігів — нічого не робимо (запит лишається) */
    }
    save_last("");                                          /* далі щось виконується — запит забуваємо */
    if (is_top(pd, line)) { run_top(pd); return MODE_EXIT; }   /* рядок-підказка */
    unsigned int local;
    int i = find_sub(pd, line, &local);
    if (i >= 0 && i == pd->files_sub && (mretv & MENU_CUSTOM_ACTION)) {   /* Shift+Enter на файлі → показати у теці */
        int st = 0;
        char *disp = mode_get_display_value(pd->subs[i], local, &st, NULL, TRUE);   /* «назва   ·  ~/тека» */
        char *sep = disp ? g_strrstr(disp, "   ·  ") : NULL;
        if (sep) {
            *sep = '\0';
            char *dir = rofi_expand_path(sep + strlen("   ·  "));
            char *path = g_build_filename(dir, disp, NULL);
            run_script("reveal", path);
            g_free(dir); g_free(path);
        }
        g_free(disp);
        return MODE_EXIT;
    }
    return i >= 0 ? mode_result(pd->subs[i], mretv, input, local) : MODE_EXIT;
}

static int uni_match(const Mode *sw, rofi_int_matcher **tokens, unsigned int line) {
    UniPrivate *pd = mode_get_private_data(sw);
    if (is_top(pd, line)) return pd->kind != TOP_NONE;      /* підказка видна завжди, коли є */
    unsigned int local;
    int i = find_sub(pd, line, &local);
    if (i < 0) return 0;
    switch (pd->prefix) {                                   /* символ режиму обмежує список */
    case PX_TERM: case PX_WEB: return 0;                    /* лише підказка */
    case PX_ACTS:  if (i != pd->acts_sub)  return 0; break;
    case PX_FILES: if (i != pd->files_sub) return 0; break;
    default:
        if (i == pd->files_sub) return 0;                   /* файли — лише через ' */
        if (pd->empty && i != pd->win_sub) return 0;        /* нічого не шукаємо — лише відкриті вікна */
        break;
    }
    return mode_token_match(pd->subs[i], tokens, local);
}

static char *uni_display(const Mode *sw, unsigned int line, int *state, GList **attrs, int get_entry) {
    UniPrivate *pd = mode_get_private_data(sw);
    if (is_top(pd, line)) {
        /* поле очищено (rofi тоді не викликає preprocess) → прибрати живий рядок */
        ViewHead *v = view();
        char *txt = (v && v->text) ? textbox_get_text(v->text) : NULL;
        if (txt && *txt == '\0') queue_sync_top(pd);
        g_free(txt);
        *state |= 1 << 1;                   /* стан «active» — інший колір у темі */
        return get_entry ? g_strdup(pd->top_text ? pd->top_text : "") : NULL;
    }
    if (line == 0 && !pd->only_win) {        /* поле очищено (preprocess тоді не викликається) → лише вікна */
        ViewHead *v = view();
        char *txt = (v && v->text) ? textbox_get_text(v->text) : NULL;
        if (txt && *txt == '\0') queue_sync_top(pd);
        g_free(txt);
    }
    unsigned int local;
    int i = find_sub(pd, line, &local);
    if (i < 0) return get_entry ? g_strdup("") : NULL;
    return mode_get_display_value(pd->subs[i], local, state, attrs, get_entry);
}

static cairo_surface_t *uni_icon(const Mode *sw, unsigned int line, unsigned int height) {
    UniPrivate *pd = mode_get_private_data(sw);
    if (is_top(pd, line)) {
        const char *name = pd->kind == TOP_CALC ? "accessories-calculator" : pd->kind == TOP_URL ? "internet-web-browser" :
                           pd->kind == TOP_PATH ? (pd->top_is_dir ? "folder-open" : "system-file-manager") :
                           pd->kind == TOP_TERM ? "utilities-terminal" : pd->kind == TOP_WEB ? "system-search" : NULL;
        return name ? rofi_icon_fetcher_get(rofi_icon_fetcher_query(name, (int)height)) : NULL;
    }
    unsigned int local;
    int i = find_sub(pd, line, &local);
    return i >= 0 ? mode_get_icon(pd->subs[i], local, height) : NULL;
}

static char *uni_completion(const Mode *sw, unsigned int line) {
    UniPrivate *pd = mode_get_private_data(sw);
    return g_strdup(pd->input ? pd->input : "");
}

/* Викликається на КОЖНЕ натискання клавіші */
static char *uni_preprocess(Mode *sw, const char *input) {
    UniPrivate *pd = mode_get_private_data(sw);
    if (g_getenv("UNI_DEBUG")) g_printerr("uni preprocess: '%s' sel=%d sel_text='%s'\n", input, pd->sel_active, pd->sel_text ? pd->sel_text : "");
    const char *in = input ? input : "";
    char *effective = g_strdup(in);

    /* Невиконаний минулий запит підставляє launcher.sh (-filter) — він звичайний активний текст:
     * список одразу фільтрується за ним, можна дописувати. */
    if (pd->sel_active && g_strcmp0(in, pd->sel_text) != 0) {
        /* текст був «виділений» і змінився → нове введення замінює старе */
        size_t sl = strlen(pd->sel_text);
        char *repl = NULL;
        if (g_str_has_prefix(in, pd->sel_text)) repl = g_strdup(in + sl);   /* надруковано / вставлено в кінці */
        else if (strlen(in) < sl)              repl = g_strdup("");         /* Backspace / Delete → стерти все */
        pd->sel_active = FALSE;
        if (repl) {
            g_free(effective); effective = repl;
            g_free(pd->pending_text); pd->pending_text = g_strdup(repl);
            g_idle_add(idle_replace_text, NULL);
        }
    }

    g_free(pd->input); pd->input = g_strdup(effective);
    save_last(effective);

    /* Символ режиму: «/» перед наявним шляхом (/etc/hosts) — це шлях, а не пошук */
    Prefix px; int plen = detect_prefix(effective, &px);
    gboolean dummy;
    char *p;
    if (px == PX_WEB && effective[0] == '/' && (p = try_path(effective, &dummy))) { g_free(p); px = PX_NONE; plen = 0; }
    /* Старий запит ще не активний (не редагували стрілками) — шукати нічого: лише відкриті вікна */
    char *pattern = pd->sel_active ? g_strdup("") : g_strdup(effective + plen);
    if (pd->sel_active) { px = PX_NONE; }
    pd->empty = *pattern == '\0' && px == PX_NONE;
    /* Почали шукати → запустити програми й дії; «'» → ще й файли (ліниво, див. ensure_subs) */
    if (!pd->empty && ensure_subs(pd, px == PX_FILES)) queue_sync_top(pd);
    if (pd->empty != pd->only_win) queue_sync_top(pd);
    if (px != pd->prefix) { pd->prefix = px; }
    update_top(pd, pattern);
    if ((pd->kind != TOP_NONE) != pd->has_top || (pd->kind != TOP_NONE && pd->want_last != pd->top_last)) queue_sync_top(pd);
    g_free(effective);
    return pattern;                                         /* фільтруємо вже без символу */
}

static char *uni_message(const Mode *sw) {
    UniPrivate *pd = mode_get_private_data(sw);
    return (pd && pd->panel) ? g_strdup(pd->panel) : NULL;
}

static void uni_destroy(Mode *sw) {
    UniPrivate *pd = mode_get_private_data(sw);
    if (!pd) return;
    if (pd->timer) g_source_remove(pd->timer);
    for (unsigned int i = 0; i < pd->n_subs; i++) if (pd->inited[i]) mode_destroy(pd->subs[i]);
    g_free(pd->subs); g_free(pd->starts); g_free(pd->lengths); g_free(pd->inited);
    g_free(pd->top_text); g_free(pd->top_value); g_free(pd->input); g_free(pd->sel_text); g_free(pd->pending_text);
    g_free(pd->panel); g_free(pd->cpu_name); g_free(pd->gpu_name); g_free(pd->gpu_dir); g_free(pd->temp_path); g_free(pd->net_if); g_free(pd->bat_dir); g_free(pd->screen);
    g_free(pd);
    g_pd = NULL;
    mode_set_private_data(sw, NULL);
}

Mode mode = {
    .abi_version        = ABI_VERSION,
    .name               = "uni",
    .cfg_name_key       = "display-uni",
    ._init              = uni_init,
    ._destroy           = uni_destroy,
    ._get_num_entries   = uni_num,
    ._result            = uni_result,
    ._token_match       = uni_match,
    ._get_display_value = uni_display,
    ._get_icon          = uni_icon,
    ._get_completion    = uni_completion,
    ._preprocess_input  = uni_preprocess,
    ._get_message       = uni_message,
    .type               = MODE_TYPE_SWITCHER,
};
