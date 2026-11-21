/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  ПЛАНШЕТ — віджет робочого столу (частина desk)              │
 * │                                                              │
 * │  Графічний планшет (Wacom і подібні; кабель або Bluetooth):  │
 * │  кожен — окрема картка, як у геймпадів. Наживо видно:        │
 * │    • де перо над поверхнею (кружечок; торкнулось — залитий,  │
 * │      гумка — червоний), нахил пера (рисочка-«хвіст»);        │
 * │    • натиск — смужка праворуч і відсоток;                    │
 * │    • кнопки пера, кнопки планшета (ExpressKeys), кільце.     │
 * │  У заголовку: заряд (Bluetooth) і затримка — скільки подія   │
 * │  йде від драйвера до віджета.                                │
 * │                                                              │
 * │  Налаштування (спільні для всіх планшетів):                  │
 * │    • робоча область: уся поверхня / як екран (пропорції) /   │
 * │      своя — потягнути мишкою на малюнку (пропорції екрана);  │
 * │    • кнопки пера: як є / як інша кнопка / гумка, поки тримаю /│
 * │      скасувати / повторити / рука (Space) / вимкнено;        │
 * │    • натиск м'якше / жорсткіше; екран (якщо їх кілька).      │
 * │  Область і натиск — у ~/.config/hypr/tablet-settings.lua,     │
 * │  Hyprland застосовує їх через hypr/tablet.lua (TabletApply). │
 * │  Кнопки пера: якщо хоч одна не «як є» — віджет перехоплює    │
 * │  перо (EVIOCGRAB) і віддає Hyprland віртуальну копію (uinput)│
 * │  з переставленими кнопками; клавіші — окрема віртуальна      │
 * │  клавіатура. Окремий потік — без затримок від малювання.     │
 * │  Віджет закрився / впав — ядро само віддає справжнє перо.    │
 * │                                                              │
 * │  Не підключено — картка «підключити» (devconn.c).            │
 * │  Джерело — /dev/input/event* (udev-правило                   │
 * │  72-tablet-uaccess.rules).                                   │
 * │  Сховати / показати — desk.sh toggle tablet                  │
 * │  Перевірка вигляду без планшета: DESK_TEST_TABLET=1          │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include "desk.h"
#include <linux/input.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <poll.h>
#include <pthread.h>
#include <json-glib/json-glib.h>

#define AREA_W   252          /* ширина малюнка (= вміст картки) */
#define BAR_W    8            /* смужка натиску праворуч */
#define BAR_GAP  10
#define MAXB     24           /* кнопок на планшеті — не більше */
#define DRAW_MS  16           /* не частіше ~60 кадрів/с */
#define REMAP_PHYS "desk-remap"   /* наші віртуальні пристрої — самі себе не підхоплюємо */
#define RGB(c)      cairo_set_source_rgb(cr, c)
#define RGBA(c, a)  cairo_set_source_rgba(cr, c, a)
#define NBITS(x) ((((x) - 1) / (sizeof(long) * 8)) + 1)
static gboolean bit(const unsigned long *a, int b) { return (a[b / (sizeof(long) * 8)] >> (b % (sizeof(long) * 8))) & 1; }

/* Що робить кнопка пера */
enum { M_ASIS, M_B1, M_B2, M_ERASER, M_UNDO, M_REDO, M_PAN, M_NONE, M_N };
static const char *M_LABEL[M_N] = { "Як є", "Як кнопка 1", "Як кнопка 2", "Гумка", "Скасувати", "Повторити", "Рука", "Вимкнено" };
static const char *M_TIP = "Як є — як задумано в програмі\nГумка — поки тримаю кнопку, перо стирає\n"
                           "Скасувати — Ctrl+Z, Повторити — Ctrl+Shift+Z\nРука — пробіл, поки тримаю (рухати полотно)";
static const char *M_KEY[M_N] = { "asis", "b1", "b2", "eraser", "undo", "redo", "pan", "none" };

/* Пересилання з перехопленням (кнопки пера) */
typedef struct {
    int real_fd, ui_fd, kb_fd, pipe_r, pipe_w, stop_r, stop_w;
    pthread_t th;
    int map[2];
    gboolean pen_in, touch, erasing;
} Fwd;

typedef struct {
    char *key, *name, *mac, *pen_path, *pad_path, *sys_dir;
    int bus, pen_fd, pad_fd;
    guint pen_watch, pad_watch;
    struct input_absinfo ax, ay, ap, atx, aty, ar;
    struct input_id id;
    gboolean has_p, has_tilt, has_ring, has_b3;
    int x, y, p, tx, ty, ring;
    gboolean prox, rubber, touch, b1, b2, b3;
    int nbtn, btn[MAXB];
    gboolean on[MAXB];
    int battery;
    gboolean charging, fake;
    double lat_ms;
    guint seq;
    Fwd *fwd;
    Card *card;
    int slot;
    GtkWidget *area, *sub, *bat, *lat, *hint;
    gboolean vis, dirty;
    gboolean dragging;            /* малює свою область мишкою */
    double d0x, d0y, d1x, d1y;    /* частки поверхні 0…1 */
} Tab;

static GPtrArray *tabs;
static guint draw_src, rebuild_src;
static GDBusConnection *sysbus;
static void rebuild(void);
static void fwd_apply_all(void);

/* ═════════════ НАЛАШТУВАННЯ ═════════════ */
enum { A_FULL, A_SCREEN, A_CUSTOM };
typedef struct {
    char output[64];
    int area;                     /* A_* */
    double cx, cy, cw, ch;        /* своя область — частки поверхні */
    gboolean left, relative;      /* у картці не показуються; лишаються у файлі */
    double press;
    int map[2];
} Sets;
static Sets S = { "", A_SCREEN, 0, 0, 1, 1, FALSE, FALSE, 0, { M_ASIS, M_ASIS } };
static guint save_src;

static char *sets_path(void) { return g_build_filename(g_get_home_dir(), ".config/hypr/tablet-settings.lua", NULL); }

static void sets_load(void) {
    char *f = sets_path(), *s = NULL;
    if (g_file_get_contents(f, &s, NULL, NULL)) {
        char **ls = g_strsplit(s, "\n", -1);
        for (char **l = ls; *l; l++) {
            char *t = g_strstrip(*l), *eq = strchr(t, '=');
            if (!eq || g_str_has_prefix(t, "--")) continue;
            char *v = g_strstrip(eq + 1);
            #define NUM() g_ascii_strtod(v, NULL)
            if (g_str_has_prefix(t, "output")) {
                char *a = strchr(v, '"'), *b = a ? strchr(a + 1, '"') : NULL;
                if (a && b) g_strlcpy(S.output, a + 1, MIN((gsize)(b - a), sizeof S.output));
            }
            else if (g_str_has_prefix(t, "aspect"))   S.area = g_str_has_prefix(v, "true") ? A_SCREEN : A_FULL;   /* старий формат */
            else if (g_str_has_prefix(t, "area_mode")) S.area = CLAMP((int)NUM(), 0, 2);
            else if (g_str_has_prefix(t, "custom")) {   /* custom = { x, y, w, h } */
                double a[4];
                char *p = strchr(v, '{');
                if (p && sscanf(p + 1, "%lf , %lf , %lf , %lf", &a[0], &a[1], &a[2], &a[3]) == 4) {
                    S.cx = CLAMP(a[0], 0, 1); S.cy = CLAMP(a[1], 0, 1); S.cw = CLAMP(a[2], 0.05, 1); S.ch = CLAMP(a[3], 0.05, 1);
                }
            }
            else if (g_str_has_prefix(t, "left"))     S.left = g_str_has_prefix(v, "true");
            else if (g_str_has_prefix(t, "relative")) S.relative = g_str_has_prefix(v, "true");
            else if (g_str_has_prefix(t, "pressure")) S.press = CLAMP(NUM(), -1, 1);
            else if (g_str_has_prefix(t, "button1") || g_str_has_prefix(t, "button2")) {
                int i = t[6] == '2';
                for (int m = 0; m < M_N; m++) if (strstr(v, M_KEY[m]) == v + 1) S.map[i] = m;
            }
            #undef NUM
        }
        g_strfreev(ls);
    }
    g_free(s); g_free(f);
}

/* Екрани (логічний розмір: з масштабом і поворотом) */
typedef struct { char name[64]; double x, y, w, h; } Mon;
static Mon mons[8];
static int n_mons;
static void read_monitors(void) {
    n_mons = 0;
    char *out = NULL;
    char *argv[] = { "hyprctl", "monitors", "-j", NULL };
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL)) return;
    JsonParser *jp = json_parser_new();
    if (json_parser_load_from_data(jp, out, -1, NULL) && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(jp))) {
        JsonArray *a = json_node_get_array(json_parser_get_root(jp));
        for (guint i = 0; i < json_array_get_length(a) && n_mons < 8; i++) {
            JsonObject *o = json_array_get_object_element(a, i);
            Mon *m = &mons[n_mons++];
            g_strlcpy(m->name, json_object_get_string_member_with_default(o, "name", "?"), sizeof m->name);
            double sc = json_object_get_double_member_with_default(o, "scale", 1);
            double w = json_object_get_int_member_with_default(o, "width", 1920) / sc;
            double h = json_object_get_int_member_with_default(o, "height", 1080) / sc;
            if (json_object_get_int_member_with_default(o, "transform", 0) % 2) { double t = w; w = h; h = t; }
            m->x = json_object_get_int_member_with_default(o, "x", 0);
            m->y = json_object_get_int_member_with_default(o, "y", 0);
            m->w = w; m->h = h;
        }
    }
    g_object_unref(jp); g_free(out);
}
static gboolean screen_size(double *w, double *h) {   /* вибраний екран або рамка навколо всіх */
    if (!n_mons) read_monitors();
    if (!n_mons) return FALSE;
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < n_mons; i++) {
        if (*S.output && strcmp(S.output, mons[i].name)) continue;
        x0 = MIN(x0, mons[i].x); y0 = MIN(y0, mons[i].y);
        x1 = MAX(x1, mons[i].x + mons[i].w); y1 = MAX(y1, mons[i].y + mons[i].h);
    }
    if (x1 <= x0) return FALSE;
    *w = x1 - x0; *h = y1 - y0;
    return TRUE;
}

static double span(struct input_absinfo *a) { return MAX(1, a->maximum - a->minimum); }
static double mm(struct input_absinfo *a) { return span(a) / (a->resolution > 0 ? a->resolution : 100); }   /* роздільність: одиниць на мм */

/* Співвідношення сторін екрана в частках поверхні планшета (ширина/висота у «частках») */
static double screen_ratio_frac(Tab *t) {
    double sw, sh;
    if (!screen_size(&sw, &sh)) return mm(&t->ax) / mm(&t->ay);
    return (sw / sh) * mm(&t->ay) / mm(&t->ax);
}

/* Робоча область в частках поверхні (x, y, w, h); уся поверхня — 0,0,1,1 */
static void area_frac(Tab *t, double *x, double *y, double *w, double *h) {
    *x = 0; *y = 0; *w = 1; *h = 1;
    if (!t || S.relative) return;
    if (S.area == A_CUSTOM) { *x = S.cx; *y = S.cy; *w = S.cw; *h = S.ch; return; }
    if (S.area != A_SCREEN) return;
    double r = screen_ratio_frac(t);           /* w/h у частках */
    if (r < 1) { *w = r; *x = (1 - r) / 2; } else { *h = 1 / r; *y = (1 - *h) / 2; }
}

static gboolean do_save(gpointer d) {
    save_src = 0;
    Tab *t = tabs->len ? tabs->pdata[0] : NULL;
    double fx, fy, fw, fh, aw = 0, ah = 0, ax = 0, ay = 0;
    area_frac(t, &fx, &fy, &fw, &fh);
    if (t && !(fx == 0 && fy == 0 && fw == 1 && fh == 1)) {
        double tw = mm(&t->ax), th = mm(&t->ay);
        aw = fw * tw; ah = fh * th; ax = fx * tw; ay = fy * th;
    }
    char b[10][G_ASCII_DTOSTR_BUF_SIZE];
    char *txt = g_strdup_printf(
        "-- Налаштування графічного планшета. Пише віджет «Планшет» на столі (hypr/src/tablet.c),\n"
        "-- застосовує hypr/tablet.lua: hyprctl eval \"TabletApply()\"\n"
        "return {\n"
        "    output    = \"%s\",   -- екран: \"\" — усі, або назва (eDP-1, HDMI-A-1…)\n"
        "    area_mode = %d,   -- робоча область: 0 — уся поверхня, 1 — як екран (пропорції), 2 — своя\n"
        "    custom    = { %s, %s, %s, %s },   -- своя область: x, y, ширина, висота (частки поверхні)\n"
        "    left      = %s,   -- для лівші: планшет повернуто на 180°\n"
        "    relative  = %s,   -- як миша: перо рухає курсор відносно, а не в точку\n"
        "    pressure  = %s,   -- натиск: −1 м'якше … 0 звичайно … 1 жорсткіше\n"
        "    button1   = \"%s\",   -- кнопка пера 1: asis b1 b2 eraser undo redo pan none\n"
        "    button2   = \"%s\",   -- кнопка пера 2\n"
        "    area_w = %s, area_h = %s,   -- робоча область, мм (0 — уся поверхня) — для Hyprland\n"
        "    area_x = %s, area_y = %s,   -- її відступ від верхнього лівого кута, мм\n"
        "}\n",
        S.output, S.area,
        g_ascii_formatd(b[0], sizeof b[0], "%.4f", S.cx), g_ascii_formatd(b[1], sizeof b[1], "%.4f", S.cy),
        g_ascii_formatd(b[2], sizeof b[2], "%.4f", S.cw), g_ascii_formatd(b[3], sizeof b[3], "%.4f", S.ch),
        S.left ? "true" : "false", S.relative ? "true" : "false",
        g_ascii_formatd(b[4], sizeof b[4], "%.2f", S.press), M_KEY[S.map[0]], M_KEY[S.map[1]],
        g_ascii_formatd(b[5], sizeof b[5], "%.1f", aw), g_ascii_formatd(b[6], sizeof b[6], "%.1f", ah),
        g_ascii_formatd(b[7], sizeof b[7], "%.1f", ax), g_ascii_formatd(b[8], sizeof b[8], "%.1f", ay));
    char *f = sets_path(), *tmp = g_strconcat(f, ".new", NULL);
    if (g_file_set_contents(tmp, txt, -1, NULL)) g_rename(tmp, f);
    g_free(tmp); g_free(f); g_free(txt);
    hypr_eval("TabletApply()");
    return G_SOURCE_REMOVE;
}
static void mark_all(void);
static void save_soon(int ms) {               /* повзунок тягнуть — не писати щокроку */
    if (save_src) g_source_remove(save_src);
    save_src = g_timeout_add(ms, do_save, NULL);
    mark_all();
}

/* ═════════════ ПЕРЕХОПЛЕННЯ ПЕРА: кнопки ═════════════ */
static void emit(GArray *out, int type, int code, int val) {
    struct input_event e = { .type = type, .code = code, .value = val };
    g_array_append_val(out, e);
}
static void flush_out(int fd, GArray *out) {
    if (out->len && write(fd, out->data, out->len * sizeof(struct input_event)) < 0) { /* пристрій зник — потік завершиться */ }
    g_array_set_size(out, 0);
}
static void keys(int kb, gboolean down, const int *codes) {   /* натиснути / відпустити комбінацію */
    GArray *o = g_array_new(FALSE, FALSE, sizeof(struct input_event));
    int n = 0;
    while (codes[n]) n++;
    for (int i = 0; i < n; i++) emit(o, EV_KEY, codes[down ? i : n - 1 - i], down);
    emit(o, EV_SYN, SYN_REPORT, 0);
    flush_out(kb, o);
    g_array_free(o, TRUE);
}
static const int K_UNDO[] = { KEY_LEFTCTRL, KEY_Z, 0 }, K_REDO[] = { KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_Z, 0 }, K_PAN[] = { KEY_SPACE, 0 };

static void *fwd_main(void *d) {
    Fwd *f = d;
    struct pollfd p[2] = { { f->real_fd, POLLIN, 0 }, { f->stop_r, POLLIN, 0 } };
    struct input_event ev[64];
    GArray *out = g_array_new(FALSE, FALSE, sizeof(struct input_event));
    for (;;) {
        if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (p[1].revents) break;
        if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        ssize_t n = read(f->real_fd, ev, sizeof ev);
        if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; break; }
        if (n == 0) break;
        if (write(f->pipe_w, ev, n) < 0) { /* віджет не встигає — малюнок пропустить кадр, перо — ні */ }
        for (size_t i = 0; i < (size_t)n / sizeof *ev; i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_KEY) {
                if (e->code == BTN_TOUCH) f->touch = e->value;
                if (e->code == BTN_TOOL_PEN) {
                    f->pen_in = e->value;
                    if (f->erasing) {                    /* у режимі гумки перо «вийшло» — гумка теж */
                        if (!e->value) { emit(out, EV_KEY, BTN_TOOL_RUBBER, 0); f->erasing = FALSE; }
                        continue;
                    }
                }
                int b = e->code == BTN_STYLUS ? 0 : e->code == BTN_STYLUS2 ? 1 : -1;
                if (b >= 0) {
                    int m = f->map[b];
                    switch (m) {
                    case M_ASIS: emit(out, EV_KEY, e->code, e->value); break;
                    case M_B1:   emit(out, EV_KEY, BTN_STYLUS, e->value); break;
                    case M_B2:   emit(out, EV_KEY, BTN_STYLUS2, e->value); break;
                    case M_ERASER:                       /* перо ↔ гумка: спершу відірвати, потім змінити інструмент */
                        if (e->value && f->pen_in && !f->erasing) {
                            if (f->touch) { emit(out, EV_KEY, BTN_TOUCH, 0); emit(out, EV_SYN, SYN_REPORT, 0); }
                            emit(out, EV_KEY, BTN_TOOL_PEN, 0); emit(out, EV_SYN, SYN_REPORT, 0);
                            emit(out, EV_KEY, BTN_TOOL_RUBBER, 1);
                            if (f->touch) emit(out, EV_KEY, BTN_TOUCH, 1);
                            f->erasing = TRUE;
                        } else if (!e->value && f->erasing) {
                            if (f->touch) { emit(out, EV_KEY, BTN_TOUCH, 0); emit(out, EV_SYN, SYN_REPORT, 0); }
                            emit(out, EV_KEY, BTN_TOOL_RUBBER, 0); emit(out, EV_SYN, SYN_REPORT, 0);
                            if (f->pen_in) emit(out, EV_KEY, BTN_TOOL_PEN, 1);
                            if (f->touch && f->pen_in) emit(out, EV_KEY, BTN_TOUCH, 1);
                            f->erasing = FALSE;
                        }
                        break;
                    case M_UNDO: if (e->value && f->kb_fd >= 0) { keys(f->kb_fd, TRUE, K_UNDO); keys(f->kb_fd, FALSE, K_UNDO); } break;
                    case M_REDO: if (e->value && f->kb_fd >= 0) { keys(f->kb_fd, TRUE, K_REDO); keys(f->kb_fd, FALSE, K_REDO); } break;
                    case M_PAN:  if (f->kb_fd >= 0) keys(f->kb_fd, e->value, K_PAN); break;
                    default: break;                      /* вимкнено */
                    }
                    continue;
                }
            }
            g_array_append_val(out, *e);
            if (e->type == EV_SYN && e->code == SYN_REPORT) flush_out(f->ui_fd, out);
        }
        flush_out(f->ui_fd, out);
    }
    g_array_free(out, TRUE);
    close(f->pipe_w); f->pipe_w = -1;            /* віджет побачить кінець: перо зникло (або нас зупинили) */
    return NULL;
}

/* Віртуальна копія пристрою: ті самі осі, кнопки (+ гумка), властивості, назва й id (щоб libwacom упізнав) */
static int ui_clone(int src, const char *name, struct input_id *id) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    unsigned long evb[NBITS(EV_CNT)] = { 0 }, keyb[NBITS(KEY_CNT)] = { 0 }, absb[NBITS(ABS_CNT)] = { 0 },
                  mscb[NBITS(MSC_CNT)] = { 0 }, props[NBITS(INPUT_PROP_CNT)] = { 0 };
    ioctl(src, EVIOCGBIT(0, sizeof evb), evb);
    ioctl(src, EVIOCGBIT(EV_KEY, sizeof keyb), keyb);
    ioctl(src, EVIOCGBIT(EV_ABS, sizeof absb), absb);
    ioctl(src, EVIOCGBIT(EV_MSC, sizeof mscb), mscb);
    ioctl(src, EVIOCGPROP(sizeof props), props);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int c = 0; c < KEY_CNT; c++) if (bit(keyb, c)) ioctl(fd, UI_SET_KEYBIT, c);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_RUBBER);
    ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS);
    ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS2);
    if (bit(evb, EV_ABS)) {
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        for (int c = 0; c < ABS_CNT; c++) if (bit(absb, c)) {
            struct uinput_abs_setup s = { .code = c };
            ioctl(src, EVIOCGABS(c), &s.absinfo);
            ioctl(fd, UI_ABS_SETUP, &s);
        }
    }
    if (bit(evb, EV_MSC)) {
        ioctl(fd, UI_SET_EVBIT, EV_MSC);
        for (int c = 0; c < MSC_CNT; c++) if (bit(mscb, c)) ioctl(fd, UI_SET_MSCBIT, c);
    }
    for (int c = 0; c < INPUT_PROP_CNT; c++) if (bit(props, c)) ioctl(fd, UI_SET_PROPBIT, c);
    ioctl(fd, UI_SET_PHYS, REMAP_PHYS);
    struct uinput_setup us = { .id = *id };
    g_strlcpy(us.name, name, UINPUT_MAX_NAME_SIZE);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
    return fd;
}
static int ui_keyboard(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (const int *k = (const int[]){ KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_Z, KEY_SPACE, 0 }; *k; k++) ioctl(fd, UI_SET_KEYBIT, *k);
    ioctl(fd, UI_SET_PHYS, REMAP_PHYS);
    struct uinput_setup us = { .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1, .product = 0x1 } };
    g_strlcpy(us.name, "Desk tablet keys", UINPUT_MAX_NAME_SIZE);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
    return fd;
}

static gboolean on_pen(int fd, GIOCondition cond, gpointer d);
static gboolean need_remap(void) { return S.map[0] != M_ASIS || S.map[1] != M_ASIS; }

static void fwd_stop(Tab *t) {
    Fwd *f = t->fwd;
    if (!f) return;
    if (t->pen_watch) { g_source_remove(t->pen_watch); t->pen_watch = 0; }
    if (write(f->stop_w, "x", 1) < 0) { }
    pthread_join(f->th, NULL);
    if (f->ui_fd >= 0) { ioctl(f->ui_fd, UI_DEV_DESTROY); close(f->ui_fd); }
    if (f->kb_fd >= 0) { ioctl(f->kb_fd, UI_DEV_DESTROY); close(f->kb_fd); }
    close(f->real_fd);                           /* перехоплення знято — Hyprland знову бачить справжнє перо */
    if (f->pipe_w >= 0) close(f->pipe_w);
    close(f->pipe_r); close(f->stop_r); close(f->stop_w);
    g_free(f);
    t->fwd = NULL;
    if (t->pen_fd >= 0) t->pen_watch = g_unix_fd_add(t->pen_fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_pen, t);
}
static void fwd_start(Tab *t) {
    if (t->fwd || t->fake || !t->pen_path || t->pen_fd < 0 || !need_remap()) return;
    Fwd *f = g_new0(Fwd, 1);
    f->map[0] = S.map[0]; f->map[1] = S.map[1];
    f->kb_fd = -1;
    f->real_fd = open(t->pen_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int pp[2], sp[2];
    if (f->real_fd < 0 || ioctl(f->real_fd, EVIOCGRAB, 1) < 0) { if (f->real_fd >= 0) close(f->real_fd); g_free(f); return; }
    int clk = CLOCK_MONOTONIC;
    ioctl(f->real_fd, EVIOCSCLOCKID, &clk);
    f->ui_fd = ui_clone(f->real_fd, t->name, &t->id);
    if (f->ui_fd < 0 || pipe2(pp, O_NONBLOCK | O_CLOEXEC) < 0 || pipe2(sp, O_CLOEXEC) < 0) {
        if (f->ui_fd >= 0) close(f->ui_fd);
        close(f->real_fd); g_free(f);
        return;
    }
    gboolean need_kb = FALSE;
    for (int i = 0; i < 2; i++) need_kb |= S.map[i] == M_UNDO || S.map[i] == M_REDO || S.map[i] == M_PAN;
    if (need_kb) f->kb_fd = ui_keyboard();
    f->pipe_r = pp[0]; f->pipe_w = pp[1]; f->stop_r = sp[0]; f->stop_w = sp[1];
    if (t->pen_watch) { g_source_remove(t->pen_watch); t->pen_watch = 0; }
    t->fwd = f;
    pthread_create(&f->th, NULL, fwd_main, f);
    t->pen_watch = g_unix_fd_add(f->pipe_r, G_IO_IN | G_IO_HUP | G_IO_ERR, on_pen, t);   /* малюнок — з потоку */
}
static void fwd_apply_all(void) {
    for (guint i = 0; i < tabs->len; i++) { Tab *t = tabs->pdata[i]; fwd_stop(t); fwd_start(t); }
}

/* ═════════════ МАЛЮНОК ═════════════ */
static void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}
static void surf_size(Tab *t, double *bw, double *bh) {   /* ширина повна, висота — за пропорціями планшета */
    double tw = mm(&t->ax), th = mm(&t->ay);
    *bw = AREA_W - BAR_W - BAR_GAP;
    *bh = *bw * th / tw;
    if (*bh > 160) { *bh = 160; *bw = *bh * tw / th; }
    if (*bh < 60) *bh = 60;
}
static int pad_rows(Tab *t) {
    if (!t->pad_path || (!t->nbtn && !t->has_ring)) return 0;
    int room = AREA_W - (t->has_ring ? 40 : 0), per = MAX(1, (room + 6) / 22);
    return t->nbtn ? (t->nbtn + per - 1) / per : 1;
}
static int area_height(Tab *t) {
    double bw, bh;
    surf_size(t, &bw, &bh);
    int rows = pad_rows(t);
    return (int)bh + 2 + 12 + 16 + (rows ? 12 + MAX(rows * 22 - 6, t->has_ring ? 32 : 0) : 0);
}
static void chip(cairo_t *cr, double *x, double y, const char *s, gboolean on, gboolean red) {
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    double w = e.x_advance + 14;
    rrect(cr, *x, y, w, 16, 8);
    if (on) { if (red) RGB(TH_RED_RGB); else RGB(TH_ACCENT_RGB); }
    else RGB(TH_HOVER_RGB);
    cairo_fill(cr);
    if (on) RGB(TH_BG_RGB); else RGB(TH_MUTED_RGB);
    cairo_move_to(cr, *x + 7, y + 12);
    cairo_show_text(cr, s);
    *x += w + 6;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, Tab *t) {
    double bw, bh;
    surf_size(t, &bw, &bh);
    double x0 = 0, y0 = 1;
    rrect(cr, x0 + 0.5, y0 + 0.5, bw - 1, bh - 1, 10);                  /* поверхня */
    RGB(TH_HOVER_RGB); cairo_fill_preserve(cr);
    RGB(TH_LINE_RGB); cairo_set_line_width(cr, 1); cairo_stroke(cr);
    /* робоча область (або та, що зараз тягнуть) */
    double fx, fy, fw, fh;
    if (t->dragging) { fx = MIN(t->d0x, t->d1x); fy = MIN(t->d0y, t->d1y); fw = fabs(t->d1x - t->d0x); fh = fabs(t->d1y - t->d0y); }
    else area_frac(t, &fx, &fy, &fw, &fh);
    if (!(fx == 0 && fy == 0 && fw == 1 && fh == 1)) {
        cairo_rectangle(cr, x0 + fx * bw + 2.5, y0 + fy * bh + 2.5, MAX(1, fw * bw - 5), MAX(1, fh * bh - 5));
        RGBA(TH_ACCENT_RGB, 0.10); cairo_fill_preserve(cr);
        static const double dash[] = { 4, 3 };
        cairo_set_dash(cr, dash, 2, 0);
        RGBA(TH_ACCENT_RGB, 0.7); cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
    if (S.area == A_CUSTOM && !t->dragging && !t->prox) {               /* підказка */
        cairo_select_font_face(cr, TH_FONT_UI, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9);
        const char *s = "потягни мишкою, щоб вибрати";
        cairo_text_extents_t e;
        cairo_text_extents(cr, s, &e);
        RGBA(TH_MUTED_RGB, 0.8);
        cairo_move_to(cr, x0 + (bw - e.x_advance) / 2, y0 + bh - 8);
        cairo_show_text(cr, s);
    }
    double pr = t->has_p ? CLAMP((t->p - t->ap.minimum) / span(&t->ap), 0, 1) : (t->touch ? 1 : 0);
    if (!t->touch) pr = 0;
    if (t->prox) {                                                      /* перо */
        double px_ = CLAMP((t->x - t->ax.minimum) / span(&t->ax), 0, 1), py_ = CLAMP((t->y - t->ay.minimum) / span(&t->ay), 0, 1);
        if (S.left) { px_ = 1 - px_; py_ = 1 - py_; }
        double px = x0 + 4 + px_ * (bw - 8), py = y0 + 4 + py_ * (bh - 8);
        if (t->has_tilt) {
            double dx = t->tx / MAX(1.0, (double)t->atx.maximum), dy = t->ty / MAX(1.0, (double)t->aty.maximum);
            if (S.left) { dx = -dx; dy = -dy; }
            cairo_move_to(cr, px, py);
            cairo_line_to(cr, px + dx * 26, py + dy * 26);
            RGBA(TH_MUTED_RGB, 0.8); cairo_set_line_width(cr, 2); cairo_stroke(cr);
        }
        cairo_arc(cr, px, py, 3.5 + pr * 6, 0, 2 * G_PI);
        if (t->rubber) RGB(TH_RED_RGB); else if (t->touch) RGB(TH_ACCENT_RGB); else RGB(TH_TEXT_RGB);
        if (t->touch) cairo_fill(cr); else { cairo_set_line_width(cr, 1.5); cairo_stroke(cr); }
    }
    double bx = x0 + bw + BAR_GAP;                                       /* смужка натиску */
    rrect(cr, bx, y0, BAR_W, bh, BAR_W / 2.0);
    RGB(TH_HOVER_RGB); cairo_fill(cr);
    if (pr > 0.01) {
        double fh2 = MAX(BAR_W, pr * bh);
        rrect(cr, bx, y0 + bh - fh2, BAR_W, fh2, BAR_W / 2.0);
        if (t->rubber) RGB(TH_RED_RGB); else RGB(TH_ACCENT_RGB);
        cairo_fill(cr);
    }
    cairo_select_font_face(cr, TH_FONT_UI, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    double y = y0 + bh + 12, x = x0;
    chip(cr, &x, y, "Кн. 1", t->b1, FALSE);
    chip(cr, &x, y, "Кн. 2", t->b2, FALSE);
    if (t->has_b3) chip(cr, &x, y, "Кн. 3", t->b3, FALSE);
    chip(cr, &x, y, "Гумка", t->prox && t->rubber, TRUE);
    char s[32];
    g_snprintf(s, sizeof s, "%d%%", (int)(pr * 100 + 0.5));
    cairo_select_font_face(cr, TH_FONT_MONO, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    RGB(TH_MUTED_RGB);
    cairo_move_to(cr, AREA_W - e.x_advance, y + 12);
    cairo_show_text(cr, s);
    if (pad_rows(t)) {                                                  /* кнопки планшета й кільце */
        y += 16 + 12;
        x = x0;
        if (t->has_ring) {
            double cx = x + 16, cy = y + 16;
            cairo_arc(cr, cx, cy, 13, 0, 2 * G_PI);
            RGB(TH_LINE_RGB); cairo_set_line_width(cr, 5); cairo_stroke(cr);
            if (t->ring > t->ar.minimum) {
                double a = (t->ring - t->ar.minimum) / (span(&t->ar) + 1) * 2 * G_PI - G_PI / 2;
                cairo_arc(cr, cx + 13 * cos(a), cy + 13 * sin(a), 4, 0, 2 * G_PI);
                RGB(TH_ACCENT_RGB); cairo_fill(cr);
            }
            x += 40;
        }
        double sx = x;
        for (int i = 0; i < t->nbtn; i++) {
            if (sx + 16 > AREA_W + 0.5) { sx = x; y += 22; }
            rrect(cr, sx, y, 16, 16, 4);
            if (t->on[i]) RGB(TH_ACCENT_RGB); else RGB(TH_HOVER_RGB);
            cairo_fill_preserve(cr);
            if (t->on[i]) RGB(TH_ACCENT_RGB); else RGB(TH_LINE_RGB);
            cairo_set_line_width(cr, 1); cairo_stroke(cr);
            sx += 22;
        }
    }
    return TRUE;
}

/* Своя область: потягнути мишкою на малюнку поверхні (пропорції екрана тримаються) */
static gboolean in_surface(Tab *t, double ex, double ey, double *fx, double *fy) {
    double bw, bh;
    surf_size(t, &bw, &bh);
    *fx = CLAMP(ex / bw, 0, 1); *fy = CLAMP((ey - 1) / bh, 0, 1);
    return ex >= 0 && ex <= bw && ey >= 0 && ey <= bh + 2;
}
static void drag_fix(Tab *t) {               /* другий кут — так, щоб пропорції як в екрана */
    double r = screen_ratio_frac(t);
    double dx = t->d1x - t->d0x, dy = t->d1y - t->d0y;
    double w = fabs(dx), h = fabs(dy);
    if (w / MAX(h, 1e-6) > r) h = w / r; else w = h * r;
    double sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    double mx = sx > 0 ? 1 - t->d0x : t->d0x, my = sy > 0 ? 1 - t->d0y : t->d0y;   /* не виходити за край */
    if (w > mx) { w = mx; h = w / r; }
    if (h > my) { h = my; w = h * r; }
    t->d1x = t->d0x + sx * w; t->d1y = t->d0y + sy * h;
}
static gboolean on_area_press(GtkWidget *w, GdkEventButton *e, Tab *t) {
    if (e->button != 1 || S.area != A_CUSTOM) return FALSE;              /* інакше — перетягування картки */
    double fx, fy;
    if (!in_surface(t, e->x, e->y, &fx, &fy)) return FALSE;
    t->dragging = TRUE;
    t->d0x = t->d1x = fx; t->d0y = t->d1y = fy;
    gtk_widget_queue_draw(w);
    return TRUE;
}
static gboolean on_area_motion(GtkWidget *w, GdkEventMotion *e, Tab *t) {
    if (!t->dragging) return FALSE;
    in_surface(t, e->x, e->y, &t->d1x, &t->d1y);
    drag_fix(t);
    gtk_widget_queue_draw(w);
    return TRUE;
}
static gboolean on_area_release(GtkWidget *w, GdkEventButton *e, Tab *t) {
    if (!t->dragging) return FALSE;
    t->dragging = FALSE;
    double x = MIN(t->d0x, t->d1x), y = MIN(t->d0y, t->d1y), cw = fabs(t->d1x - t->d0x), ch = fabs(t->d1y - t->d0y);
    if (cw > 0.05 && ch > 0.05) { S.cx = x; S.cy = y; S.cw = cw; S.ch = ch; save_soon(50); }
    gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean flush(gpointer d) {
    draw_src = 0;
    for (guint i = 0; i < tabs->len; i++) {
        Tab *t = tabs->pdata[i];
        if (t->dirty && t->area && t->vis) { t->dirty = FALSE; gtk_widget_queue_draw(t->area); }
    }
    return G_SOURCE_REMOVE;
}
static void mark(Tab *t) {
    t->dirty = TRUE;
    if (!draw_src && t->vis) draw_src = g_timeout_add(DRAW_MS, flush, NULL);
}
static void mark_all(void) { for (guint i = 0; i < tabs->len; i++) mark(tabs->pdata[i]); }

/* ═════════════ ЗАРЯД, ЗАТРИМКА, ЗАГОЛОВОК ═════════════ */
static void read_battery(Tab *t) {
    if (t->fake) return;
    t->battery = -1;
    if (t->sys_dir) {                                    /* драйвер wacom: power_supply біля HID-пристрою */
        char *dir = g_build_filename(t->sys_dir, "power_supply", NULL);
        GDir *d = g_dir_open(dir, 0, NULL);
        const char *n = d ? g_dir_read_name(d) : NULL;
        if (n) {
            char *f = g_build_filename(dir, n, "capacity", NULL), *s = NULL;
            if (g_file_get_contents(f, &s, NULL, NULL)) t->battery = atoi(s);
            g_free(s); g_free(f);
            f = g_build_filename(dir, n, "status", NULL);
            if (g_file_get_contents(f, &s, NULL, NULL)) t->charging = g_str_has_prefix(s, "Charging");
            g_free(s); g_free(f);
        }
        if (d) g_dir_close(d);
        g_free(dir);
    }
    if (t->battery < 0 && t->mac && sysbus) {             /* Bluetooth LE: служба заряду через BlueZ */
        char *m = g_strdup(t->mac);
        g_strdelimit(m, ":", '_');
        char *path = g_strdup_printf("/org/bluez/hci0/dev_%s", m);
        GVariant *r = g_dbus_connection_call_sync(sysbus, "org.bluez", path, "org.freedesktop.DBus.Properties", "Get",
                                                  g_variant_new("(ss)", "org.bluez.Battery1", "Percentage"), G_VARIANT_TYPE("(v)"),
                                                  G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
        if (r) { GVariant *v; g_variant_get(r, "(v)", &v); t->battery = g_variant_get_byte(v); g_variant_unref(v); g_variant_unref(r); }
        g_free(path); g_free(m);
    }
}
static void fill_head(Tab *t) {
    if (t->sub) {
        GString *s = g_string_new(t->bus == BUS_BLUETOOTH ? "Bluetooth" : "Кабель (USB)");
        if (t->charging) g_string_append(s, " · заряджається");
        if (t->fwd) g_string_append(s, " · кнопки змінено");
        gtk_label_set_text(GTK_LABEL(t->sub), s->str);
        g_string_free(s, TRUE);
    }
    if (t->bat) {
        if (t->battery >= 0) {
            static const char *BI[] = { "\U000F007A", "\U000F007B", "\U000F007C", "\U000F007D", "\U000F007E", "\U000F007F", "\U000F0080", "\U000F0081", "\U000F0082", "\U000F0079" };
            char *b = g_strdup_printf("%s %d%%", BI[CLAMP(t->battery / 10 - (t->battery == 100), 0, 9)], t->battery);
            gtk_label_set_text(GTK_LABEL(t->bat), b);
            g_free(b);
            gtk_widget_set_tooltip_text(t->bat, t->charging ? "Заряд (заряджається)" : "Заряд");
        } else gtk_label_set_text(GTK_LABEL(t->bat), t->bus == BUS_BLUETOOTH ? "— %" : "");
    }
}
static gboolean battery_tick(gpointer d) {
    for (guint i = 0; i < tabs->len; i++) { Tab *t = tabs->pdata[i]; read_battery(t); fill_head(t); }
    return G_SOURCE_CONTINUE;
}
static gboolean lat_tick(gpointer d) {                   /* раз на секунду — не щокадру */
    for (guint i = 0; i < tabs->len; i++) {
        Tab *t = tabs->pdata[i];
        if (!t->lat) continue;
        char s[24];
        if (t->lat_ms > 0) g_snprintf(s, sizeof s, "%.1f мс", t->lat_ms); else g_strlcpy(s, "— мс", sizeof s);
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(t->lat)), s)) gtk_label_set_text(GTK_LABEL(t->lat), s);
    }
    return G_SOURCE_CONTINUE;
}

static char *friendly(const char *n) {                   /* назва без « Pen» / « Pad» */
    if (n && g_str_has_prefix(n, "Wacom Co.,Ltd. ")) n += strlen("Wacom Co.,Ltd. ");   /* кабель: «Wacom Co.,Ltd. Wacom One…» */
    char *s = g_strdup(n ? n : "Планшет");
    for (const char **w = (const char *[]){ " Pen", " Pad", " stylus", " Stylus", NULL }; *w; w++)
        if (g_str_has_suffix(s, *w)) s[strlen(s) - strlen(*w)] = 0;
    return s;
}

/* ═════════════ КАРТКА ═════════════ */
static void on_disconnect(GtkButton *b, Tab *t) {
    if (!t->mac) return;
    char *argv[] = { "bluetoothctl", "disconnect", t->mac, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    gtk_button_set_label(b, "Відключаю…");
}
static void on_press(GtkRange *r, gpointer d) { S.press = gtk_range_get_value(r); save_soon(400); }
static void on_output(GtkButton *b, gpointer d) {
    g_strlcpy(S.output, g_object_get_data(G_OBJECT(b), "out"), sizeof S.output);
    save_soon(50);
    rebuild();
}
static void on_area_mode(GtkButton *b, gpointer d) {
    int m = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "mode"));
    if (m == S.area) return;
    S.area = m;
    if (m == A_CUSTOM && S.cw >= 0.999 && S.ch >= 0.999 && tabs->len) {   /* ще не вибирали — почати з «як екран» */
        area_frac(NULL, &S.cx, &S.cy, &S.cw, &S.ch);
        S.area = A_SCREEN;
        area_frac(tabs->pdata[0], &S.cx, &S.cy, &S.cw, &S.ch);
        S.area = A_CUSTOM;
    }
    save_soon(50);
    rebuild();
}
static void on_map(GtkComboBox *c, gpointer d) {
    int i = GPOINTER_TO_INT(d), m = gtk_combo_box_get_active(c);
    if (m < 0 || m == S.map[i]) return;
    S.map[i] = m;
    save_soon(50);
    fwd_apply_all();
    for (guint k = 0; k < tabs->len; k++) fill_head(tabs->pdata[k]);
}

static GtkWidget *seg_row(GtkWidget *box, const char *cls) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    add_class(row, cls);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return row;
}
static GtkWidget *seg_btn(GtkWidget *row, const char *label, gboolean active) {
    GtkWidget *b = gtk_button_new_with_label(label);
    add_class(b, "ui-btn");
    if (active) add_class(b, "active");
    gtk_widget_set_can_focus(b, FALSE);
    gtk_box_pack_start(GTK_BOX(row), b, TRUE, TRUE, 0);
    return b;
}

static void build(Tab *t) {
    GtkWidget *box = t->card->card;
    char *nm = friendly(t->name);
    GtkWidget *head = ui_header(t->card, "\U000F0F49", nm, NULL, &t->sub);
    g_free(nm);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);         /* праворуч: заряд, під ним затримка */
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
    t->bat = gtk_label_new("");
    add_class(t->bat, "pad-bat");
    gtk_label_set_xalign(GTK_LABEL(t->bat), 1);
    t->lat = gtk_label_new("— мс");
    add_class(t->lat, "pad-lat");
    gtk_label_set_xalign(GTK_LABEL(t->lat), 1);
    gtk_widget_set_tooltip_text(t->lat, "Затримка: скільки подія пера йде від драйвера до віджета\n"
                                        "(шлях Bluetooth до драйвера сюди не входить)");
    gtk_box_pack_start(GTK_BOX(right), t->bat, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), t->lat, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(head), right, FALSE, FALSE, 0);
    fill_head(t);

    t->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(t->area, AREA_W, area_height(t));
    gtk_widget_set_halign(t->area, GTK_ALIGN_CENTER);
    gtk_widget_add_events(t->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(t->area, "draw", G_CALLBACK(on_draw), t);
    g_signal_connect(t->area, "button-press-event", G_CALLBACK(on_area_press), t);
    g_signal_connect(t->area, "motion-notify-event", G_CALLBACK(on_area_motion), t);
    g_signal_connect(t->area, "button-release-event", G_CALLBACK(on_area_release), t);
    gtk_box_pack_start(GTK_BOX(box), t->area, FALSE, FALSE, 0);

    ui_section(box, "Робоча область");
    GtkWidget *seg = seg_row(box, "tb-outs");
    static const char *AL[] = { "Уся", "Як екран", "Своя" };
    static const char *AT[] = { "Уся поверхня планшета (фігури трохи розтягнуті)",
                                "Найбільша область з пропорціями екрана: коло лишається колом",
                                "Вибери область сам: потягни мишкою на малюнку планшета" };
    for (int m = 0; m < 3; m++) {
        GtkWidget *b = seg_btn(seg, AL[m], S.area == m);
        gtk_widget_set_tooltip_text(b, AT[m]);
        g_object_set_data(G_OBJECT(b), "mode", GINT_TO_POINTER(m));
        g_signal_connect(b, "clicked", G_CALLBACK(on_area_mode), NULL);
    }
    read_monitors();
    if (n_mons > 1) {                                    /* вибір екрана — лише коли є з чого */
        GtkWidget *row = seg_row(box, "tb-outs");
        for (int i = -1; i < n_mons; i++) {
            const char *o = i < 0 ? "" : mons[i].name;
            GtkWidget *b = seg_btn(row, i < 0 ? "Усі екрани" : o, !strcmp(S.output, o));
            g_object_set_data_full(G_OBJECT(b), "out", g_strdup(o), g_free);
            g_signal_connect(b, "clicked", G_CALLBACK(on_output), NULL);
        }
    }

    ui_section(box, "Кнопки пера");
    for (int i = 0; i < 2; i++) {
        GtkWidget *row = ui_row_box(NULL, ui_text(i ? "Кнопка 2" : "Кнопка 1", i ? "верхня" : "нижня", NULL, NULL));
        add_class(row, "tb-row");
        GtkWidget *c = gtk_combo_box_text_new();
        for (int m = 0; m < M_N; m++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), M_LABEL[m]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(c), S.map[i]);
        gtk_widget_set_valign(c, GTK_ALIGN_CENTER);
        gtk_widget_set_can_focus(c, FALSE);
        gtk_widget_set_size_request(c, 124, -1);
        gtk_widget_set_tooltip_text(c, M_TIP);
        GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(c));     /* довга назва не розтягує картку */
        for (GList *l = cells; l; l = l->next) g_object_set(l->data, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 8, NULL);
        g_list_free(cells);
        add_class(c, "tb-map");
        g_signal_connect(c, "changed", G_CALLBACK(on_map), GINT_TO_POINTER(i));
        gtk_box_pack_end(GTK_BOX(row), c, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    }

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(col, "tb-row");
    GtkWidget *lab = gtk_label_new("Натиск");
    add_class(lab, "t-subtitle");
    add_class(lab, "ui-section");
    gtk_label_set_xalign(GTK_LABEL(lab), 0);
    GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -1, 1, 0.05);
    gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
    gtk_scale_add_mark(GTK_SCALE(sc), -1, GTK_POS_BOTTOM, "м'якше");
    gtk_scale_add_mark(GTK_SCALE(sc), 0, GTK_POS_BOTTOM, NULL);
    gtk_scale_add_mark(GTK_SCALE(sc), 1, GTK_POS_BOTTOM, "жорсткіше");
    gtk_range_set_value(GTK_RANGE(sc), S.press);
    gtk_widget_set_can_focus(sc, FALSE);
    gtk_widget_set_tooltip_text(sc, "М'якше — повний натиск настає раніше.\nЖорсткіше — найлегший дотик (до 5%) не малює.");
    add_class(sc, "tb-press");
    g_signal_connect(sc, "value-changed", G_CALLBACK(on_press), NULL);
    gtk_box_pack_start(GTK_BOX(col), lab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), sc, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), col, FALSE, FALSE, 0);

    if (t->bus == BUS_USB) gtk_box_pack_start(GTK_BOX(box), btlink_new(t->name, t->mac, "tablet"), FALSE, FALSE, 0);
    gboolean bt = t->bus == BUS_BLUETOOTH && t->mac;
    GtkWidget *acts = ui_actions(box);
    GtkWidget *b = gtk_button_new_with_label(bt ? "\U000F00B2  Відключити" : "\U000F0553  Підключено кабелем");
    add_class(b, "ui-btn");
    add_class(b, "pad-off");
    gtk_widget_set_sensitive(b, bt);
    gtk_widget_set_can_focus(b, FALSE);
    g_signal_connect(b, "clicked", G_CALLBACK(on_disconnect), t);
    gtk_box_pack_start(GTK_BOX(acts), b, TRUE, TRUE, 0);
}

static gboolean tab_on(void) {
    char *f = g_build_filename(g_get_home_dir(), ".config/hypr/tablet.off", NULL);
    gboolean on = !g_file_test(f, G_FILE_TEST_EXISTS);
    g_free(f);
    return on;
}
static void card_free(Tab *t) {
    if (!t->card) return;
    cards_unregister(t->card);
    gtk_widget_destroy(t->card->win);
    g_free((char *)t->card->pos_name);
    g_free(t->card);
    t->card = NULL; t->area = t->sub = t->bat = t->lat = NULL; t->slot = 0;
}

static gboolean recheck(gpointer d) {
    for (guint i = 0; i < tabs->len; i++) {
        Tab *t = tabs->pdata[i];
        if (!t->card) continue;
        gboolean v = g_getenv("DESK_TEST_OVERLAY") ? TRUE : card_visible(t->card);
        if (v != t->vis) { t->vis = v; if (v) mark(t); }
    }
    return G_SOURCE_REMOVE;
}
static void recheck_soon(void) { g_timeout_add(120, recheck, NULL); }

static gint by_seq(gconstpointer a, gconstpointer b) {
    const Tab *x = *(Tab *const *)a, *y = *(Tab *const *)b;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}
static gboolean do_rebuild(gpointer d) {
    rebuild_src = 0;
    gboolean on = tab_on(), any = FALSE;
    g_ptr_array_sort(tabs, by_seq);
    for (guint i = 0; i < tabs->len; i++) {
        Tab *t = tabs->pdata[i];
        gboolean want = on && t->pen_path;
        if (!want || (t->card && t->slot != (int)i + 1)) card_free(t);
        if (!want) continue;
        any = TRUE;
        if (!t->card) {
            t->slot = i + 1;
            Card *c = t->card = g_new0(Card, 1);
            c->pos_name = g_strdup_printf("desktablet%d.pos", t->slot);
            c->off_name = "tablet.off";
            if (!load_pos(c)) { c->mright = 420 + (t->slot - 1) * (CARD_W + GRID); c->mtop = 440; }
            c->moved = recheck_soon;
            card_init(c, "desktop-tablet", "tablet");
            t->vis = TRUE;
            g_timeout_add(300, recheck, NULL);
        } else {
            GList *kids = gtk_container_get_children(GTK_CONTAINER(t->card->card));
            for (GList *l = kids; l; l = l->next) gtk_widget_destroy(l->data);
            g_list_free(kids);
        }
        build(t);
        gtk_widget_show_all(t->card->win);
        gtk_window_resize(GTK_WINDOW(t->card->win), 1, 1);
        mark(t);
    }
    devconn_online("tablet", any);
    cards_relayout();
    return G_SOURCE_REMOVE;
}
static void rebuild(void) { if (!rebuild_src) rebuild_src = g_idle_add(do_rebuild, NULL); }
void tablet_apply(void) { if (tabs) rebuild(); }   /* SIGHUP (desk.sh toggle tablet) */

/* ═════════════ ПОДІЇ ═════════════ */
static void free_tab(Tab *t) {
    g_free(t->key); g_free(t->name); g_free(t->mac); g_free(t->sys_dir);
    g_free(t);
}
static void drop_part(Tab *t, gboolean pen) {   /* пристрій зник; обидва зникли — планшета нема */
    if (pen) {
        fwd_stop(t);
        if (t->pen_watch) { g_source_remove(t->pen_watch); }
        t->pen_watch = 0;
        if (t->pen_fd >= 0) close(t->pen_fd);
        t->pen_fd = -1;
        g_clear_pointer(&t->pen_path, g_free);
    } else {
        if (t->pad_fd >= 0) close(t->pad_fd);
        t->pad_fd = -1; t->pad_watch = 0;
        g_clear_pointer(&t->pad_path, g_free);
    }
    if (!t->pen_path && !t->pad_path) {
        if (t->bus == BUS_USB) btlink_unplugged(t->name, t->mac);   /* кабель витягнули — по Bluetooth */
        g_ptr_array_remove(tabs, t);
        card_free(t);
        free_tab(t);
    }
    rebuild();
}

static void pen_sync(Tab *t) {
    unsigned long k[NBITS(KEY_CNT)] = { 0 };
    ioctl(t->pen_fd, EVIOCGKEY(sizeof k), k);
    t->rubber = bit(k, BTN_TOOL_RUBBER);
    t->prox = bit(k, BTN_TOOL_PEN) || t->rubber;
    t->touch = bit(k, BTN_TOUCH);
    t->b1 = bit(k, BTN_STYLUS); t->b2 = bit(k, BTN_STYLUS2); t->b3 = bit(k, BTN_STYLUS3);
    struct input_absinfo a;
    if (!ioctl(t->pen_fd, EVIOCGABS(ABS_X), &a)) t->x = a.value;
    if (!ioctl(t->pen_fd, EVIOCGABS(ABS_Y), &a)) t->y = a.value;
    if (t->has_p && !ioctl(t->pen_fd, EVIOCGABS(ABS_PRESSURE), &a)) t->p = a.value;
}

static gboolean on_pen(int fd, GIOCondition cond, gpointer d) {
    Tab *t = d;
    struct input_event ev[64];
    ssize_t n;
    while ((n = read(fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < (size_t)n / sizeof *ev; i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_ABS) switch (e->code) {
                case ABS_X: t->x = e->value; break;
                case ABS_Y: t->y = e->value; break;
                case ABS_PRESSURE: t->p = e->value; break;
                case ABS_TILT_X: t->tx = e->value; break;
                case ABS_TILT_Y: t->ty = e->value; break;
            } else if (e->type == EV_KEY) switch (e->code) {
                case BTN_TOOL_PEN: t->prox = e->value; if (e->value) t->rubber = FALSE; break;
                case BTN_TOOL_RUBBER: t->prox = e->value; t->rubber = e->value; break;
                case BTN_TOUCH: t->touch = e->value; break;
                case BTN_STYLUS: t->b1 = e->value; break;
                case BTN_STYLUS2: t->b2 = e->value; break;
                case BTN_STYLUS3: t->b3 = e->value; break;
            } else if (e->type == EV_SYN) {
                if (e->code == SYN_DROPPED) pen_sync(t);
                else if (e->code == SYN_REPORT) {
                    double ev_us = e->input_event_sec * 1e6 + e->input_event_usec;   /* монотонний час (EVIOCSCLOCKID) */
                    double lat = (g_get_monotonic_time() - ev_us) / 1000.0;
                    if (lat >= 0 && lat < 1000) t->lat_ms = t->lat_ms > 0 ? t->lat_ms * 0.9 + lat * 0.1 : lat;
                    mark(t);
                }
            }
        }
    }
    if ((n < 0 && errno != EAGAIN) || n == 0 || (cond & (G_IO_HUP | G_IO_ERR))) {
        t->pen_watch = 0;                         /* це джерело зараз зникне саме */
        drop_part(t, TRUE);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_pad(int fd, GIOCondition cond, gpointer d) {
    Tab *t = d;
    struct input_event ev[64];
    ssize_t n;
    while ((n = read(fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < (size_t)n / sizeof *ev; i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_KEY) {
                for (int b = 0; b < t->nbtn; b++) if (t->btn[b] == e->code) t->on[b] = e->value;
            } else if (e->type == EV_ABS && e->code == ABS_WHEEL) t->ring = e->value;
            else if (e->type == EV_SYN && e->code == SYN_REPORT) mark(t);
        }
    }
    if ((n < 0 && errno != EAGAIN) || n == 0 || (cond & (G_IO_HUP | G_IO_ERR))) { t->pad_watch = 0; drop_part(t, FALSE); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

/* ═════════════ ПОЯВА ПРИСТРОЇВ ═════════════ */
static char *get_str(int fd, int which) {
    char buf[256] = "";
    int r = -1;
    if (which == 0) r = ioctl(fd, EVIOCGNAME(sizeof buf - 1), buf);
    if (which == 1) r = ioctl(fd, EVIOCGPHYS(sizeof buf - 1), buf);
    if (which == 2) r = ioctl(fd, EVIOCGUNIQ(sizeof buf - 1), buf);
    if (r < 0) buf[0] = 0;
    return g_strdup(buf);
}
static gboolean is_mac(const char *s) {
    if (!s || strlen(s) != 17) return FALSE;
    for (int i = 0; i < 17; i++) if (i % 3 == 2 ? s[i] != ':' : !g_ascii_isxdigit(s[i])) return FALSE;
    return TRUE;
}
static char *dev_key(const char *uniq, const char *phys) {   /* перо й кнопки одного планшета */
    if (uniq && *uniq) return g_ascii_strdown(uniq, -1);
    char *k = g_strdup(phys ? phys : "");
    char *sl = strrchr(k, '/');
    if (sl && g_str_has_prefix(sl, "/input")) *sl = 0;
    return k;
}
static int classify(int fd, const char *name) {          /* 1 — перо, 2 — кнопки планшета, 0 — інше */
    unsigned long keyb[NBITS(KEY_CNT)] = { 0 }, absb[NBITS(ABS_CNT)] = { 0 };
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), keyb);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
    if (bit(keyb, BTN_TOOL_PEN) && bit(absb, ABS_X) && bit(absb, ABS_Y)) return 1;
    if (bit(keyb, BTN_TOOL_FINGER) || bit(keyb, BTN_SOUTH) || bit(keyb, BTN_TOUCH)) return 0;
    if (bit(absb, ABS_X) && (bit(keyb, BTN_0) || bit(keyb, BTN_STYLUS)) && (g_str_has_suffix(name, " Pad") || bit(absb, ABS_MISC)))
        return 2;
    return 0;
}
static char *sys_dir_of(const char *path) {              /* тека HID-пристрою в sysfs (там power_supply) */
    char *base = g_path_get_basename(path);
    char *link = g_strdup_printf("/sys/class/input/%s/device", base);
    char *real = realpath(link, NULL), *res = NULL;
    for (int up = 0; real && up < 4 && !res; up++) {
        char *ps = g_build_filename(real, "power_supply", NULL);
        if (g_file_test(ps, G_FILE_TEST_IS_DIR)) res = g_strdup(real);
        g_free(ps);
        char *sl = strrchr(real, '/');
        if (!sl || sl == real) break;
        *sl = 0;
    }
    free(real); g_free(link); g_free(base);
    return res;
}
static Tab *find_by_key(const char *k) {
    for (guint i = 0; i < tabs->len; i++) { Tab *t = tabs->pdata[i]; if (!g_strcmp0(t->key, k)) return t; }
    return NULL;
}

static int try_device(const char *path) {
    for (guint i = 0; i < tabs->len; i++) {
        Tab *t = tabs->pdata[i];
        if (!g_strcmp0(t->pen_path, path) || !g_strcmp0(t->pad_path, path)) return 1;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == EACCES || errno == EPERM ? -1 : 0;
    char *name = get_str(fd, 0), *phys = get_str(fd, 1);
    int kind = g_str_has_prefix(phys, REMAP_PHYS) ? 0 : classify(fd, name);   /* наша ж віртуальна копія — ні */
    if (!kind) { close(fd); g_free(name); g_free(phys); return 0; }
    char *uniq = get_str(fd, 2), *k = dev_key(uniq, phys);
    Tab *t = find_by_key(k);
    if (!t) {
        static guint seq;
        t = g_new0(Tab, 1);
        t->key = g_strdup(k); t->pen_fd = t->pad_fd = -1; t->battery = -1; t->seq = ++seq;
        ioctl(fd, EVIOCGID, &t->id);
        t->bus = t->id.bustype;
        if (is_mac(uniq)) t->mac = g_ascii_strup(uniq, -1);
        g_ptr_array_add(tabs, t);
    }
    if (!t->sys_dir) t->sys_dir = sys_dir_of(path);
    int clk = CLOCK_MONOTONIC;
    ioctl(fd, EVIOCSCLOCKID, &clk);                       /* час подій — монотонний (для затримки) */
    if (kind == 1 && !t->pen_path) {
        unsigned long absb[NBITS(ABS_CNT)] = { 0 }, keyb[NBITS(KEY_CNT)] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), keyb);
        ioctl(fd, EVIOCGABS(ABS_X), &t->ax);
        ioctl(fd, EVIOCGABS(ABS_Y), &t->ay);
        if ((t->has_p = bit(absb, ABS_PRESSURE))) ioctl(fd, EVIOCGABS(ABS_PRESSURE), &t->ap);
        if ((t->has_tilt = bit(absb, ABS_TILT_X) && bit(absb, ABS_TILT_Y))) {
            ioctl(fd, EVIOCGABS(ABS_TILT_X), &t->atx);
            ioctl(fd, EVIOCGABS(ABS_TILT_Y), &t->aty);
        }
        t->has_b3 = bit(keyb, BTN_STYLUS3);
        ioctl(fd, EVIOCGID, &t->id);
        g_free(t->name); t->name = g_strdup(name);
        t->pen_path = g_strdup(path); t->pen_fd = fd;
        pen_sync(t);
        t->pen_watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_pen, t);
        fwd_start(t);                                     /* кнопки пера переставлені — перехопити */
    } else if (kind == 2 && !t->pad_path) {
        unsigned long absb[NBITS(ABS_CNT)] = { 0 }, keyb[NBITS(KEY_CNT)] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), keyb);
        static const int R[][2] = { { BTN_0, BTN_9 }, { BTN_LEFT, BTN_TASK }, { BTN_BASE, BTN_BASE6 }, { BTN_A, BTN_Z } };
        t->nbtn = 0;
        for (guint r = 0; r < G_N_ELEMENTS(R); r++)
            for (int c = R[r][0]; c <= R[r][1] && t->nbtn < MAXB; c++) if (bit(keyb, c)) t->btn[t->nbtn++] = c;
        if ((t->has_ring = bit(absb, ABS_WHEEL))) ioctl(fd, EVIOCGABS(ABS_WHEEL), &t->ar);
        if (!t->name) t->name = g_strdup(name);
        t->pad_path = g_strdup(path); t->pad_fd = fd;
        t->pad_watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_pad, t);
    } else close(fd);
    read_battery(t);
    rebuild();
    if (kind == 1) save_soon(300);                        /* новий планшет — область за його розміром */
    g_free(uniq); g_free(phys); g_free(k); g_free(name);
    return 1;
}

typedef struct { char *path; int tries; } Pending;
static gboolean retry_device(gpointer d) {               /* щойно з'явився: udev ще може ставити права */
    Pending *pd = d;
    if (try_device(pd->path) == -1 && ++pd->tries < 8) return G_SOURCE_CONTINUE;
    g_free(pd->path); g_free(pd);
    return G_SOURCE_REMOVE;
}
static void on_dir(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer d) {
    if (ev != G_FILE_MONITOR_EVENT_CREATED && ev != G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) return;
    char *path = g_file_get_path(f), *base = g_path_get_basename(path);
    if (g_str_has_prefix(base, "event")) {
        Pending *pd = g_new0(Pending, 1);
        pd->path = g_strdup(path);
        g_timeout_add(250, retry_device, pd);
    }
    g_free(base); g_free(path);
}

static void add_fake(void) {                             /* перевірка вигляду: вигаданий Wacom One по Bluetooth */
    Tab *t = g_new0(Tab, 1);
    t->fake = TRUE; t->key = g_strdup("fake"); t->name = g_strdup("Wacom One pen tablet small Pen");
    t->pen_path = g_strdup("fake"); t->pen_fd = t->pad_fd = -1;
    t->bus = BUS_BLUETOOTH; t->mac = g_strdup("00:00:00:00:00:00"); t->battery = 76; t->lat_ms = 6.4;
    t->ax = (struct input_absinfo){ .maximum = 15200, .resolution = 100 };
    t->ay = (struct input_absinfo){ .maximum = 9500, .resolution = 100 };
    t->ap = (struct input_absinfo){ .maximum = 4095 };
    t->atx = t->aty = (struct input_absinfo){ .minimum = -64, .maximum = 63 };
    t->has_p = t->has_tilt = TRUE;
    t->x = 9800; t->y = 3600; t->p = 1700; t->tx = 30; t->ty = 25;
    t->prox = t->touch = TRUE; t->b1 = TRUE;
    t->seq = 1;
    g_ptr_array_add(tabs, t);
}

void tablet_init(void) {
    tabs = g_ptr_array_new();
    sysbus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    sets_load();
    if (g_getenv("DESK_TEST_TABLET")) add_fake();
    GDir *dir = g_dir_open("/dev/input", 0, NULL);
    const char *n;
    while (dir && (n = g_dir_read_name(dir)))
        if (g_str_has_prefix(n, "event")) { char *p = g_build_filename("/dev/input", n, NULL); try_device(p); g_free(p); }
    if (dir) g_dir_close(dir);
    GFile *f = g_file_new_for_path("/dev/input");
    GFileMonitor *mon = g_file_monitor_directory(f, G_FILE_MONITOR_NONE, NULL, NULL);
    if (mon) g_signal_connect(mon, "changed", G_CALLBACK(on_dir), NULL);
    g_object_unref(f);
    g_timeout_add_seconds(60, battery_tick, NULL);
    g_timeout_add_seconds(1, lat_tick, NULL);
    hw_listen(recheck);
    rebuild();
}
