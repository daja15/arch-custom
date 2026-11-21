/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  КОНТРОЛЕРИ — віджет робочого столу (частина desk)           │
 * │                                                              │
 * │  Для кожного підключеного геймпада — секція «Контролер N»:   │
 * │  назва, спосіб підключення (Bluetooth / кабель), заряд,      │
 * │  намальований геймпад, на якому видно наживо:                │
 * │    • натиснуті кнопки (підсвічуються), хрестовину;           │
 * │    • стіки (зсув «шапки» і натискання стіка);                │
 * │    • курки (смужка заповнюється — аналогові і цифрові);      │
 * │    • гіроскоп: нахил (кружечок з «горизонтом») і швидкість   │
 * │      обертання по осях X/Y/Z (з окремого пристрою руху).     │
 * │  Унизу секції — «Відключити» (Bluetooth; кабельні — ні).     │
 * │                                                              │
 * │  Малюнки (власні, спрощені): Joy-Con (L / R / пара),         │
 * │  Switch Pro, Switch 2 Pro, DualShock 4, DualSense; інші —    │
 * │  звичайний геймпад. Тип — за виробником і моделлю пристрою.  │
 * │                                                              │
 * │  Джерело — /dev/input/event* (evdev), без опитування: нові /  │
 * │  зниклі пристрої — через стеження за /dev/input (inotify).   │
 * │  Немає геймпадів — картки не видно.                          │
 * │  Економія: перемальовка не частіше 25 разів/с і лише коли    │
 * │  щось помітно змінилось (шум стіків і гіроскопа нижче        │
 * │  порогу не будить); картка закрита вікном — не малюється.    │
 * │  Сховати / показати — desk.sh toggle pads                    │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include "desk.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#define AREA_W     252        /* малюнок: ширина */
#define PAD_H      150        /* … висота геймпада в «одиницях малюнка» */
#define SCALE      0.78       /* … а на екрані геймпад менший (компактна картка) */
#define PAD_PX     ((int)(PAD_H * SCALE) + 2)
#define GYRO_H     56         /* смуга гіроскопа під геймпадом (лише якщо він є) */
#define DRAW_MS    20         /* не частіше 50 кадрів/с (гіроскоп має встигати) */
#define STICK_EPS  0.02       /* зсув стіка/курка, менший за це (частка ходу), не перемальовує */
#define ANG_EPS    1.5        /* кут, градусів: менша зміна не перемальовує */
#define GYRO_DEAD  2.0        /* °/с: менше (після поправки на зсув нуля) — не крутиться */
#define ACC_TRUST  0.02       /* частка «правди» акселерометра для нахилів (решта — гіроскоп) */


typedef enum { T_GENERIC, T_PRO1, T_PRO2, T_GC, T_JCL, T_JCR, T_JCPAIR, T_PS4, T_PS5 } PadType;
enum { FN, FE, FS, FW };                         /* позиції кнопок ромба: північ, схід, південь, захід */

#define NBITS(x) ((((x) - 1) / (sizeof(long) * 8)) + 1)
static gboolean bit(const unsigned long *a, int b) { return (a[b / (sizeof(long) * 8)] >> (b % (sizeof(long) * 8))) & 1; }

typedef struct {
    char *path, *imu_path, *name, *key, *mac;
    int fd, imu_fd, bus, vendor, product, slot;
    guint watch, imu_watch;
    PadType type;
    int face[4];                                 /* код evdev для кожної позиції ромба */
    unsigned long keys[NBITS(KEY_CNT)];
    gboolean has[ABS_CNT];
    struct input_absinfo ai[ABS_CNT];
    double drawn[ABS_CNT];                       /* значення на останньому малюнку (для порогу) */
    gboolean key_changed;
    /* рух */
    int imu_raw[6];
    double acc_res, gyr_res;
    int acc_i[3], acc_s[3];                      /* сирий датчик → (праворуч, вгору, до себе): індекс і знак */
    int gyr_i[3], gyr_s[3];                      /* → швидкості (нахил вперед, поворот, нахил вбік) */
    double ang[3], d_ang[3];                     /* кути: нахил вперед/назад, поворот, нахил вбік (°, −180…180); намальовані */
    double bias[3];                              /* зсув нуля гіроскопа (оцінюється, поки лежить) */
    double imu_t;                                /* час попереднього звіту, с */
    gboolean imu_ok;
    /* тачпад (DualShock 4 / DualSense) — окремий пристрій */
    char *tp_path;
    int tp_fd, tp_slot;
    guint tp_watch;
    struct input_absinfo tp_ax, tp_ay;
    int tp_id[2], tp_x[2], tp_y[2];              /* два пальці: tracking id (−1 — немає), координати */
    gboolean tp_click;
    int learn;                                   /* 1 / 2 — чекаємо натискання для GL / GR */
    guint learn_src;
    GtkWidget *learn_btn[2];
    Card *card;                                  /* окрема картка-вікно цього геймпада (NULL — не показано) */
    gboolean vis;                                /* картку не закриває вікно */
    double lat_ms;                               /* затримка / період звітів, мс (згладжено) */
    GtkWidget *lat;
    int battery;                                 /* % або -1 */
    const char *back[2];                         /* що стоїть на задніх GL / GR (підпис), NULL — свої */
    gboolean linked;                             /* справді на зв'язку (міст Switch 2 тримає пристрій і без зв'язку) */
    guint seq;                                   /* черговість появи картки (0 — не показано): за нею — номери 1, 2, … */
    gboolean dirty;
    GtkWidget *box, *num, *name_l, *conn, *bat, *area, *btn;
} Pad;

static void learn_key(Pad *p, int code);
static GPtrArray *pads;                          /* Pad* (геймпади) */
static GPtrArray *orphans;                       /* пристрої руху, для яких ще нема геймпада: char* шлях */
static guint draw_src, rebuild_src;

static void rebuild(void);
static GDBusConnection *sysbus;                  /* системна шина: BlueZ (чи підключений геймпад мосту) */

/* ═════════════ ДОПОМІЖНЕ ═════════════ */
static char *get_str(int fd, int which) {        /* назва / phys / uniq */
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
    for (int i = 0; i < 17; i++)
        if (i % 3 == 2 ? s[i] != ':' : !g_ascii_isxdigit(s[i])) return FALSE;
    return TRUE;
}

/* Ключ «той самий фізичний пристрій» (геймпад ↔ його датчик руху): uniq (серійник / MAC), інакше phys */
static char *dev_key(const char *uniq, const char *phys) {
    if (uniq && *uniq) return g_ascii_strdown(uniq, -1);
    char *k = g_strdup(phys ? phys : "");
    return k;
}

static double norm(Pad *p, int code) {           /* вісь → −1…1 */
    if (!p->has[code]) return 0;
    struct input_absinfo *a = &p->ai[code];
    double half = (a->maximum - a->minimum) / 2.0;
    if (half <= 0) return 0;
    double v = (a->value - (a->minimum + half)) / half;
    return CLAMP(v, -1, 1);
}
static double norm01(Pad *p, int code) {         /* курок → 0…1 */
    if (!p->has[code]) return 0;
    struct input_absinfo *a = &p->ai[code];
    double r = a->maximum - a->minimum;
    return r > 0 ? CLAMP((a->value - a->minimum) / r, 0, 1) : 0;
}
static gboolean key(Pad *p, int code) { return code > 0 && bit(p->keys, code); }

/* ═════════════ ТИП ГЕЙМПАДА ═════════════ */
static PadType detect(Pad *p) {
    const char *n = p->name;
    if (p->vendor == 0x054c || strstr(n, "DualSense") || strstr(n, "Wireless Controller"))
        return (p->product == 0x0ce6 || p->product == 0x0df2 || strstr(n, "DualSense")) ? T_PS5 : T_PS4;
    if (p->vendor == 0x057e || strstr(n, "Joy-Con") || strstr(n, "Pro Controller")) {
        switch (p->product) {
        case 0x2006: case 0x2067: return T_JCL;
        case 0x2007: case 0x2066: return T_JCR;
        case 0x2008: case 0x200e: return T_JCPAIR;
        case 0x2069: return T_PRO2;
        case 0x2073: return T_GC;
        case 0x2009: return T_PRO1;
        }
        if (strstr(n, "Combined")) return T_JCPAIR;
        if (strstr(n, "(L)")) return T_JCL;
        if (strstr(n, "(R)")) return T_JCR;
        if (strstr(n, "Pro Controller 2")) return T_PRO2;
        if (strstr(n, "GameCube")) return T_GC;
        return T_PRO1;
    }
    return T_GENERIC;
}

static const char *friendly(Pad *p) {
    switch (p->type) {
    case T_PRO1:   return "Nintendo Switch Pro Controller";
    case T_PRO2:   return "Nintendo Switch 2 Pro Controller";
    case T_GC:     return "Nintendo GameCube Controller";
    case T_JCL:    return p->product == 0x2067 ? "Joy-Con 2 (L)" : "Joy-Con (L)";
    case T_JCR:    return p->product == 0x2066 ? "Joy-Con 2 (R)" : "Joy-Con (R)";
    case T_JCPAIR: return "Joy-Con (пара)";
    case T_PS4:    return "DualShock 4";
    case T_PS5:    return "DualSense";
    default:       return p->name;
    }
}

/* Яка кнопка evdev де стоїть на ромбі. Switch 1 (hid-nintendo), Sony, інші — за положенням
 * (BTN_SOUTH — нижня). Switch 2 — за написом: A = BTN_SOUTH, хоча фізично A праворуч;
 * причому драйвер ядра і Bluetooth-міст (phys «ngc/…») по-різному ставлять X і Y. */
static void face_map(Pad *p) {
    int pos[4] = { BTN_NORTH, BTN_EAST, BTN_SOUTH, BTN_WEST };
    gboolean sw2 = p->vendor == 0x057e && (p->product == 0x2066 || p->product == 0x2067 ||
                                           p->product == 0x2069 || p->product == 0x2073);
    if (g_str_has_prefix(p->key, "ngc/")) sw2 = TRUE;              /* Bluetooth-міст Switch 2 */
    if (sw2) {
        gboolean bridge = g_str_has_prefix(p->key, "ngc/");   /* міст: Bluetooth і кабельний режим */
        pos[FN] = bridge ? BTN_WEST : BTN_NORTH;   /* X */
        pos[FE] = BTN_SOUTH;                       /* A */
        pos[FS] = BTN_EAST;                        /* B */
        pos[FW] = bridge ? BTN_NORTH : BTN_WEST;   /* Y */
    }
    memcpy(p->face, pos, sizeof pos);
}

/* ═════════════ МАЛЮВАННЯ ═════════════ */
#define RGB(c)  cairo_set_source_rgb(cr, c)
#define RGBA(c, a) cairo_set_source_rgba(cr, c, a)

typedef struct {
    double ls_x, ls_y, rs_x, rs_y;               /* стіки (0 — немає) */
    double f_x, f_y, d_x, d_y;                   /* ромб кнопок, хрестовина */
    double mi_x, mi_y, pl_x, pl_y, ho_x, ho_y, ca_x, ca_y;   /* −/Share, +/Options, Home/PS, Capture */
    double l_x0, l_x1, r_x0, r_x1;               /* плечові кнопки й курки (по x) */
} Layout;

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
    r = MIN(r, MIN(w, h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static Layout layout(PadType t) {
    switch (t) {
    case T_JCL:    return (Layout){ .ls_x = 126, .ls_y = 58, .d_x = 126, .d_y = 100, .mi_x = 140, .mi_y = 40,
                                    .ca_x = 132, .ca_y = 130, .l_x0 = 104, .l_x1 = 146 };
    case T_JCR:    return (Layout){ .rs_x = 126, .rs_y = 100, .f_x = 126, .f_y = 60, .pl_x = 112, .pl_y = 40,
                                    .ho_x = 120, .ho_y = 130, .r_x0 = 106, .r_x1 = 148 };
    case T_JCPAIR: return (Layout){ .ls_x = 89, .ls_y = 58, .d_x = 89, .d_y = 100, .mi_x = 103, .mi_y = 40, .ca_x = 95, .ca_y = 130,
                                    .rs_x = 163, .rs_y = 100, .f_x = 163, .f_y = 60, .pl_x = 149, .pl_y = 40, .ho_x = 157, .ho_y = 130,
                                    .l_x0 = 68, .l_x1 = 110, .r_x0 = 142, .r_x1 = 184 };
    case T_PS4: case T_PS5:
                   return (Layout){ .d_x = 70, .d_y = 58, .ls_x = 98, .ls_y = 92, .rs_x = 154, .rs_y = 92, .f_x = 182, .f_y = 58,
                                    .mi_x = 86, .mi_y = 38, .pl_x = 166, .pl_y = 38, .ho_x = 126, .ho_y = 84,
                                    .l_x0 = 46, .l_x1 = 96, .r_x0 = 156, .r_x1 = 206 };
    default:       return (Layout){ .ls_x = 70, .ls_y = 56, .d_x = 100, .d_y = 88, .rs_x = 152, .rs_y = 88, .f_x = 182, .f_y = 56,
                                    .mi_x = 106, .mi_y = 44, .pl_x = 146, .pl_y = 44, .ho_x = 140, .ho_y = 62, .ca_x = 112, .ca_y = 62,
                                    .l_x0 = 46, .l_x1 = 96, .r_x0 = 156, .r_x1 = 206 };
    }
}

static void dot(cairo_t *cr, double x, double y, double r, gboolean on) {
    cairo_new_path(cr);                           /* без лінії від попередньої точки (після тексту) */
    cairo_arc(cr, x, y, r, 0, 2 * G_PI);
    if (on) RGB(TH_ACCENT_RGB); else RGB(TH_OVERLAY_RGB);
    cairo_fill_preserve(cr);
    RGB(TH_LINE_RGB);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

static void stick(cairo_t *cr, double x, double y, double vx, double vy, gboolean click) {
    cairo_new_path(cr);
    cairo_arc(cr, x, y, 13, 0, 2 * G_PI);
    RGB(TH_BG_RGB);
    cairo_fill_preserve(cr);
    RGB(TH_LINE_RGB);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    double kx = x + vx * 7, ky = y + vy * 7;
    if (fabs(vx) > 0.12 || fabs(vy) > 0.12) {    /* куди відхилено */
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, kx, ky);
        RGB(TH_ACCENT2_RGB);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
    }
    cairo_arc(cr, kx, ky, 8, 0, 2 * G_PI);
    if (click) RGB(TH_ACCENT_RGB); else RGB(TH_HOVER_RGB);
    cairo_fill_preserve(cr);
    if (fabs(vx) > 0.12 || fabs(vy) > 0.12) RGB(TH_ACCENT2_RGB); else RGB(TH_MUTED_RGB);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
}

static void letter(cairo_t *cr, double x, double y, const char *s, gboolean on) {
    cairo_text_extents_t e;
    cairo_select_font_face(cr, "JetBrainsMono Nerd Font Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 8.5);
    cairo_text_extents(cr, s, &e);
    cairo_move_to(cr, x - e.width / 2 - e.x_bearing, y - e.height / 2 - e.y_bearing);
    if (on) RGB(TH_BG_RGB); else RGB(TH_MUTED_RGB);
    cairo_show_text(cr, s);
}

/* Значки Sony всередині кнопки: 0 △, 1 ○, 2 ✕, 3 □ */
static void sony_mark(cairo_t *cr, double x, double y, int which, gboolean on) {
    if (on) RGB(TH_BG_RGB); else RGB(TH_MUTED_RGB);
    cairo_set_line_width(cr, 1.3);
    switch (which) {
    case 0: cairo_move_to(cr, x, y - 3.6); cairo_line_to(cr, x + 3.4, y + 2.4); cairo_line_to(cr, x - 3.4, y + 2.4); cairo_close_path(cr); break;
    case 1: cairo_arc(cr, x, y, 3.2, 0, 2 * G_PI); break;
    case 2: cairo_move_to(cr, x - 3, y - 3); cairo_line_to(cr, x + 3, y + 3); cairo_move_to(cr, x + 3, y - 3); cairo_line_to(cr, x - 3, y + 3); break;
    case 3: cairo_rectangle(cr, x - 2.8, y - 2.8, 5.6, 5.6); break;
    }
    cairo_stroke(cr);
}

static void face(cairo_t *cr, Pad *p, double x, double y) {
    static const double off[4][2] = { { 0, -11 }, { 11, 0 }, { 0, 11 }, { -11, 0 } };
    static const char *NIN[4] = { "X", "A", "B", "Y" }, *GEN[4] = { "Y", "B", "A", "X" };
    gboolean sony = p->type == T_PS4 || p->type == T_PS5;
    gboolean nin = p->vendor == 0x057e || (p->type >= T_PRO1 && p->type <= T_JCPAIR);
    for (int i = 0; i < 4; i++) {
        gboolean on = key(p, p->face[i]);
        double bx = x + off[i][0], by = y + off[i][1];
        dot(cr, bx, by, 6.5, on);
        if (sony) sony_mark(cr, bx, by, i, on);
        else letter(cr, bx, by, nin ? NIN[i] : GEN[i], on);
    }
}

static void dpad(cairo_t *cr, Pad *p, double x, double y, gboolean buttons) {
    int hx = p->has[ABS_HAT0X] ? p->ai[ABS_HAT0X].value : 0, hy = p->has[ABS_HAT0Y] ? p->ai[ABS_HAT0Y].value : 0;
    gboolean on[4] = { hy < 0 || key(p, BTN_DPAD_UP), hx > 0 || key(p, BTN_DPAD_RIGHT),
                       hy > 0 || key(p, BTN_DPAD_DOWN), hx < 0 || key(p, BTN_DPAD_LEFT) };
    static const double off[4][2] = { { 0, -9 }, { 9, 0 }, { 0, 9 }, { -9, 0 } };
    for (int i = 0; i < 4; i++) {
        double bx = x + off[i][0], by = y + off[i][1];
        if (buttons) { dot(cr, bx, by, 5.5, on[i]); continue; }   /* Joy-Con: окремі кнопки */
        rrect(cr, bx - 5, by - 5, 10, 10, 2.5);
        if (on[i]) RGB(TH_ACCENT_RGB); else RGB(TH_OVERLAY_RGB);
        cairo_fill_preserve(cr);
        RGB(TH_LINE_RGB);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    if (!buttons) { cairo_rectangle(cr, x - 4, y - 4, 8, 8); RGB(TH_OVERLAY_RGB); cairo_fill(cr); }
}

/* Плечова кнопка (L/R) і курок над нею (ZL/ZR, L2/R2): курок заповнюється знизу */
static void shoulder(cairo_t *cr, double x0, double x1, double trig, gboolean bumper) {
    double w = x1 - x0;
    rrect(cr, x0 + 6, 2, w - 12, 12, 4);          /* курок */
    RGB(TH_BG_RGB);
    cairo_fill_preserve(cr);
    RGB(TH_LINE_RGB);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (trig > 0.01) {
        cairo_save(cr);
        rrect(cr, x0 + 6, 2, w - 12, 12, 4);
        cairo_clip(cr);
        cairo_rectangle(cr, x0 + 6, 14 - 12 * trig, w - 12, 12 * trig);
        RGB(TH_ACCENT2_RGB);
        cairo_fill(cr);
        cairo_restore(cr);
    }
    rrect(cr, x0, 17, w, 9, 4);                   /* плечова */
    if (bumper) RGB(TH_ACCENT_RGB); else RGB(TH_OVERLAY_RGB);
    cairo_fill_preserve(cr);
    RGB(TH_LINE_RGB);
    cairo_stroke(cr);
}

static double trig(Pad *p, int abs, int btn) { return MAX(norm01(p, abs), key(p, btn) ? 1.0 : 0.0); }

static void draw_gyro(cairo_t *cr, Pad *p) {
    double y0 = PAD_PX + 4;
    cairo_select_font_face(cr, "JetBrainsMono Nerd Font Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9);
    /* положення в просторі: коло з «горизонтом» (нахил вбік — поворот лінії, вперед/назад — її зсув) */
    double cx = 24, cy = y0 + 24, R = 21;
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, R, 0, 2 * G_PI);
    RGB(TH_BG_RGB);
    cairo_fill_preserve(cr);
    RGB(TH_LINE_RGB);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_save(cr);
    cairo_arc(cr, cx, cy, R - 1, 0, 2 * G_PI);
    cairo_clip(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, -p->d_ang[2] * G_PI / 180);
    double pitch = p->d_ang[0];                   /* перевернутий (за 90°) — горизонт іде з іншого боку */
    double sh = sin(pitch * G_PI / 180) * R;
    gboolean upside = fabs(pitch) > 90;
    cairo_rectangle(cr, -R * 2, sh, R * 4, upside ? -R * 3 : R * 3);   /* «земля» */
    RGBA(TH_ACCENT_RGB, 0.25);
    cairo_fill(cr);
    cairo_move_to(cr, -R * 2, sh);
    cairo_line_to(cr, R * 2, sh);
    RGB(TH_ACCENT_RGB);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    cairo_restore(cr);
    cairo_save(cr);                               /* стрілка повороту — куди «дивиться» геймпад */
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, -p->d_ang[1] * G_PI / 180);
    cairo_move_to(cr, 0, -R + 2); cairo_line_to(cr, -3, -R + 8); cairo_line_to(cr, 3, -R + 8); cairo_close_path(cr);
    RGB(TH_ACCENT2_RGB);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, 2, 0, 2 * G_PI);
    RGB(TH_TEXT_RGB);
    cairo_fill(cr);

    /* три шкали, 0 — посередині: вправо 0…180 (180 — правий край), вліво від лівого краю 181…360,
     * і 360 = 0 знову в центрі. Число — 0…360°. */
    static const char *AX[3] = { "Вперед", "Поворот", "Вбік" };
    double bx = 96, bw = AREA_W - bx - 36, mid = bx + bw / 2;
    char t[16];
    for (int i = 0; i < 3; i++) {
        double y = y0 + 2 + i * 17, deg = fmod(fmod(p->d_ang[i], 360) + 360, 360);
        double off = deg <= 180 ? deg / 180 : -(360 - deg) / 180;   /* −1…1 від центру */
        RGB(TH_MUTED_RGB);
        cairo_set_font_size(cr, 9);
        cairo_move_to(cr, 52, y + 9);
        cairo_show_text(cr, AX[i]);
        rrect(cr, bx, y + 2, bw, 8, 3);
        RGB(TH_BG_RGB);
        cairo_fill_preserve(cr);
        RGB(TH_LINE_RGB);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        double kx = mid + off * bw / 2;
        cairo_rectangle(cr, MIN(mid, kx), y + 3.5, fabs(kx - mid), 5);   /* заповнення від центру */
        RGBA(TH_ACCENT2_RGB, 0.55);
        cairo_fill(cr);
        for (int q = 1; q < 4; q++) {             /* 270 | 0 | 90 */
            double x = bx + bw * q / 4;
            cairo_move_to(cr, x, y + (q == 2 ? 0 : 1)); cairo_line_to(cr, x, y + (q == 2 ? 12 : 11));
            if (q == 2) RGB(TH_MUTED_RGB); else RGB(TH_FAINT_RGB);
            cairo_stroke(cr);
        }
        cairo_move_to(cr, kx, y); cairo_line_to(cr, kx, y + 12);   /* повзунок */
        RGB(TH_ACCENT2_RGB);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
        g_snprintf(t, sizeof t, "%3.0f°", deg);
        RGB(TH_TEXT_RGB);
        cairo_move_to(cr, bx + bw + 4, y + 9);
        cairo_show_text(cr, t);
    }
    cairo_set_font_size(cr, 7);                   /* підписи шкали */
    RGB(TH_DIM_RGB);
    static const char *TK[5] = { "181", "270", "0", "90", "180" };
    for (int q = 0; q < 5; q++) {
        cairo_text_extents_t e;
        cairo_text_extents(cr, TK[q], &e);
        cairo_move_to(cr, CLAMP(bx + bw * q / 4 - e.x_advance / 2, bx - 2, bx + bw - e.x_advance + 2), y0 + 2 + 3 * 17 + 4);
        cairo_show_text(cr, TK[q]);
    }
}

/* Тачпад Sony: де пальці (до двох) і натискання самої панелі */
static void draw_touch(cairo_t *cr, Pad *p, double x, double y, double w, double h) {
    if (p->tp_click) {
        rrect(cr, x, y, w, h, 6);
        RGBA(TH_ACCENT_RGB, 0.45);
        cairo_fill(cr);
    }
    for (int i = 0; i < 2; i++) {
        if (p->tp_id[i] < 0) continue;
        double rx = p->tp_ax.maximum > p->tp_ax.minimum ? (double)(p->tp_x[i] - p->tp_ax.minimum) / (p->tp_ax.maximum - p->tp_ax.minimum) : 0.5;
        double ry = p->tp_ay.maximum > p->tp_ay.minimum ? (double)(p->tp_y[i] - p->tp_ay.minimum) / (p->tp_ay.maximum - p->tp_ay.minimum) : 0.5;
        cairo_new_path(cr);
        cairo_arc(cr, x + 4 + CLAMP(rx, 0, 1) * (w - 8), y + 4 + CLAMP(ry, 0, 1) * (h - 8), 4, 0, 2 * G_PI);
        RGB(TH_ACCENT2_RGB);
        cairo_fill(cr);
    }
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, Pad *p) {
    PadType t = p->type;
    Layout L = layout(t);
    gboolean sony = t == T_PS4 || t == T_PS5;
    cairo_save(cr);                               /* геймпад — зменшений, по центру */
    cairo_translate(cr, AREA_W / 2.0, 1);
    cairo_scale(cr, SCALE, SCALE);
    cairo_translate(cr, -AREA_W / 2.0, 0);

    /* плечові кнопки й курки */
    if (L.l_x1 > 0) shoulder(cr, L.l_x0, L.l_x1, trig(p, ABS_Z, BTN_TL2), key(p, BTN_TL));
    if (L.r_x1 > 0) shoulder(cr, L.r_x0, L.r_x1, trig(p, ABS_RZ, BTN_TR2), key(p, BTN_TR));

    /* корпусу не малюємо — лише кнопки, стіки, курки (і тачпад) на своїх місцях */
    if (sony) {                                   /* сенсорна панель */
        rrect(cr, 100, 32, 52, 30, 6);
        RGB(TH_HOVER_RGB);
        cairo_fill_preserve(cr);
        RGB(TH_LINE_RGB);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        if (p->tp_path) draw_touch(cr, p, 100, 32, 52, 30);
    }
    if (t == T_PRO2) {                            /* задні кнопки GL / GR — мітки на ручках + що на них стоїть */
        dot(cr, 38, 124, 4.5, key(p, BTN_GRIPL));
        dot(cr, 214, 124, 4.5, key(p, BTN_GRIPR));
        cairo_select_font_face(cr, "JetBrainsMono Nerd Font Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 8);
        RGB(TH_MUTED_RGB);
        char s[32];
        cairo_text_extents_t e;
        for (int s2 = 0; s2 < 2; s2++) {          /* під міткою: GL / GR або що на ній стоїть */
            g_snprintf(s, sizeof s, "%s", p->back[s2] ? p->back[s2] : (s2 ? "GR" : "GL"));
            cairo_text_extents(cr, s, &e);
            cairo_move_to(cr, (s2 ? 214 : 38) - e.x_advance / 2, 140);
            cairo_show_text(cr, s);
        }
        dot(cr, 126, 76, 3.5, key(p, BTN_C));     /* C */
    }

    /* стіки: лівий — ABS_X/Y; у правого Joy-Con окремо — що є */
    if (L.ls_x > 0) stick(cr, L.ls_x, L.ls_y, norm(p, ABS_X), norm(p, ABS_Y), key(p, BTN_THUMBL));
    if (L.rs_x > 0) {
        gboolean own = p->has[ABS_RX] || t != T_JCR;
        stick(cr, L.rs_x, L.rs_y, own ? norm(p, ABS_RX) : norm(p, ABS_X), own ? norm(p, ABS_RY) : norm(p, ABS_Y),
              key(p, BTN_THUMBR));
    }
    if (L.f_x > 0) face(cr, p, L.f_x, L.f_y);
    if (L.d_x > 0) dpad(cr, p, L.d_x, L.d_y, t == T_JCL || t == T_JCPAIR);
    if (L.mi_x > 0) dot(cr, L.mi_x, L.mi_y, 3.5, key(p, BTN_SELECT));
    if (L.pl_x > 0) dot(cr, L.pl_x, L.pl_y, 3.5, key(p, BTN_START));
    if (L.ho_x > 0) dot(cr, L.ho_x, L.ho_y, sony ? 5 : 4.5, key(p, BTN_MODE));
    if (L.ca_x > 0) dot(cr, L.ca_x, L.ca_y, 3.5, key(p, BTN_Z) || key(p, KEY_RECORD));

    cairo_restore(cr);
    if (p->imu_path) draw_gyro(cr, p);           /* гіроскопа нема — і смуги нема */
    return TRUE;
}

/* ═════════════ ПЕРЕМАЛЬОВКА (не частіше DRAW_MS) ═════════════ */
static gboolean flush(gpointer d) {
    draw_src = 0;
    for (guint i = 0; i < pads->len; i++) {
        Pad *p = pads->pdata[i];
        if (p->dirty && p->area && p->vis) { p->dirty = FALSE; gtk_widget_queue_draw(p->area); }
    }
    return G_SOURCE_REMOVE;
}
static void mark(Pad *p) {
    p->dirty = TRUE;
    if (!draw_src && p->vis) draw_src = g_timeout_add(DRAW_MS, flush, NULL);
}

/* ═════════════ ЧИТАННЯ ПОДІЙ ═════════════ */
static void sync_state(Pad *p) {                 /* початковий стан і після втрати подій */
    ioctl(p->fd, EVIOCGKEY(sizeof p->keys), p->keys);
    for (int c = 0; c < ABS_CNT; c++)
        if (p->has[c]) { ioctl(p->fd, EVIOCGABS(c), &p->ai[c]); p->drawn[c] = 9; }
}

static void remove_pad(Pad *p);

static void imu_close(Pad *p) {
    if (p->imu_watch) { g_source_remove(p->imu_watch); p->imu_watch = 0; }
    if (p->imu_fd >= 0) { close(p->imu_fd); p->imu_fd = -1; }
    g_clear_pointer(&p->imu_path, g_free);
}

static gboolean on_pad(int fd, GIOCondition cond, gpointer data) {
    Pad *p = data;
    struct input_event ev[64];
    gboolean change = FALSE;
    for (;;) {
        ssize_t n = read(fd, ev, sizeof ev);
        if (n < 0 && errno == EAGAIN) break;
        if (n <= 0) { p->watch = 0; remove_pad(p); return G_SOURCE_REMOVE; }   /* пристрій зник */
        for (int i = 0; i < (int)(n / sizeof ev[0]); i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_KEY && e->code < KEY_CNT) {
                if (e->value == 1 && !p->linked) { p->linked = TRUE; rebuild(); }   /* натискання = точно на зв'язку */
                if (e->value == 1 && p->learn) learn_key(p, e->code);                /* призначаємо задню кнопку */
                unsigned long m = 1UL << (e->code % (sizeof(long) * 8));
                if (e->value) p->keys[e->code / (sizeof(long) * 8)] |= m;
                else          p->keys[e->code / (sizeof(long) * 8)] &= ~m;
                change = TRUE;
            } else if (e->type == EV_ABS && e->code < ABS_CNT) {
                p->ai[e->code].value = e->value;
                struct input_absinfo *a = &p->ai[e->code];
                double r = a->maximum - a->minimum, v = r > 0 ? (e->value - a->minimum) / r : 0;
                if (fabs(v - p->drawn[e->code]) > STICK_EPS || a->maximum - a->minimum <= 2) {
                    p->drawn[e->code] = v; change = TRUE;
                }
            } else if (e->type == EV_SYN && e->code == SYN_REPORT && !p->imu_path) {
                /* скільки подія йшла від драйвера до нас (годинник подій — CLOCK_MONOTONIC) */
                double age = g_get_monotonic_time() / 1000.0 - (e->input_event_sec * 1000.0 + e->input_event_usec / 1000.0);
                if (age >= 0 && age < 1000) p->lat_ms = p->lat_ms ? p->lat_ms * 0.8 + age * 0.2 : age;
            } else if (e->type == EV_SYN && e->code == SYN_DROPPED) {
                sync_state(p); change = TRUE;
            }
        }
    }
    if (change) mark(p);
    return G_SOURCE_CONTINUE;
}

static gboolean on_imu(int fd, GIOCondition cond, gpointer data) {
    Pad *p = data;
    struct input_event ev[64];
    for (;;) {
        ssize_t n = read(fd, ev, sizeof ev);
        if (n < 0 && errno == EAGAIN) break;
        if (n <= 0) { p->imu_watch = 0; imu_close(p); rebuild(); return G_SOURCE_REMOVE; }
        for (int i = 0; i < (int)(n / sizeof ev[0]); i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_ABS && e->code <= ABS_RZ && e->code != ABS_THROTTLE) {
                int idx = e->code <= ABS_Z ? e->code : 3 + (e->code - ABS_RX);
                if (idx >= 0 && idx < 6) p->imu_raw[idx] = e->value;
            } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
                double now = e->input_event_sec + e->input_event_usec / 1e6, dt = p->imu_ok ? now - p->imu_t : 0;
                p->imu_t = now;
                if (dt < 0 || dt > 0.2) dt = 0;
                if (dt > 0) p->lat_ms = p->lat_ms ? p->lat_ms * 0.9 + dt * 1000 * 0.1 : dt * 1000;   /* період звітів руху */
                /* у спільну систему: r — праворуч, u — вгору (з лицевого боку), b — до себе */
                double a[3], g[3];
                for (int k = 0; k < 3; k++) a[k] = p->acc_s[k] * p->imu_raw[p->acc_i[k]] / p->acc_res;
                for (int k = 0; k < 3; k++) g[k] = p->gyr_s[k] * p->imu_raw[3 + p->gyr_i[k]] / p->gyr_res;
                double r = a[0], u = a[1], b = a[2], gn = sqrt(r * r + u * u + b * b);
                /* лежить нерухомо → потроху уточнюємо зсув нуля гіроскопа */
                if (fabs(gn - 1) < 0.05 && fabs(g[0] - p->bias[0]) < 3 && fabs(g[1] - p->bias[1]) < 3 && fabs(g[2] - p->bias[2]) < 3)
                    for (int k = 0; k < 3; k++) p->bias[k] += (g[k] - p->bias[k]) * 0.02;
                for (int k = 0; k < 3; k++) { g[k] -= p->bias[k]; if (fabs(g[k]) < GYRO_DEAD) g[k] = 0; }
                double acc_pitch = atan2(-b, u) * 180 / G_PI;   /* + — верх (курки) підняли */
                double acc_roll  = atan2(-r, u) * 180 / G_PI;   /* + — правий бік донизу */
                if (!p->imu_ok) { p->ang[0] = acc_pitch; p->ang[2] = acc_roll; p->ang[1] = 0; }
                else {
                    /* гіроскоп — плавно й швидко; акселерометр — повільно тягне до правди (без дрейфу) */
                    double tgt[3] = { acc_pitch, 0, acc_roll };
                    for (int k = 0; k < 3; k++) {
                        p->ang[k] += g[k] * dt;
                        if (k != 1 && fabs(gn - 1) < 0.3) {   /* поворот акселерометр не бачить */
                            double d = remainder(tgt[k] - p->ang[k], 360.0);
                            p->ang[k] += d * ACC_TRUST;
                        }
                        p->ang[k] = remainder(p->ang[k], 360.0);
                    }
                }
                p->imu_ok = TRUE;
                gboolean ch = FALSE;
                for (int k = 0; k < 3; k++)
                    if (fabs(remainder(p->ang[k] - p->d_ang[k], 360.0)) > ANG_EPS) ch = TRUE;
                if (ch) { memcpy(p->d_ang, p->ang, sizeof p->ang); mark(p); }
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

static void imu_attach(Pad *p, const char *path, int fd) {
    imu_close(p);
    p->imu_path = g_strdup(path);
    p->imu_fd = fd;
    struct input_absinfo a;
    p->acc_res = (ioctl(fd, EVIOCGABS(ABS_X), &a) == 0 && a.resolution > 0) ? a.resolution : 4096;
    p->gyr_res = (ioctl(fd, EVIOCGABS(ABS_RX), &a) == 0 && a.resolution > 0) ? a.resolution : 1;
    /* міст Switch 2 пише resolution 164 («16,4 × 10»), а насправді 16,4 одиниці на °/с —
     * без поправки обертання видно вдесятеро слабше й «доповзає» за акселерометром із запізненням */
    if (g_str_has_prefix(p->key, "ngc/") && p->gyr_res == 164) p->gyr_res = 16.4;
    p->imu_ok = FALSE;
    p->imu_watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_imu, p);
    memset(p->ang, 0, sizeof p->ang); memset(p->bias, 0, sizeof p->bias);
    rebuild();                                   /* з'явилась смуга гіроскопа */
}

/* ═════════════ ТАЧПАД (Sony) ═════════════ */
static void tp_close(Pad *p) {
    if (p->tp_watch) { g_source_remove(p->tp_watch); p->tp_watch = 0; }
    if (p->tp_fd >= 0) { close(p->tp_fd); p->tp_fd = -1; }
    g_clear_pointer(&p->tp_path, g_free);
    p->tp_id[0] = p->tp_id[1] = -1; p->tp_click = FALSE;
}

static gboolean on_tp(int fd, GIOCondition cond, gpointer data) {
    Pad *p = data;
    struct input_event ev[64];
    gboolean ch = FALSE;
    for (;;) {
        ssize_t n = read(fd, ev, sizeof ev);
        if (n < 0 && errno == EAGAIN) break;
        if (n <= 0) { p->tp_watch = 0; tp_close(p); mark(p); return G_SOURCE_REMOVE; }
        for (int i = 0; i < (int)(n / sizeof ev[0]); i++) {
            struct input_event *e = &ev[i];
            int s = p->tp_slot;
            if (e->type == EV_ABS && e->code == ABS_MT_SLOT) p->tp_slot = e->value;
            else if (e->type == EV_ABS && s >= 0 && s < 2) {
                if (e->code == ABS_MT_TRACKING_ID) p->tp_id[s] = e->value;
                if (e->code == ABS_MT_POSITION_X)  p->tp_x[s] = e->value;
                if (e->code == ABS_MT_POSITION_Y)  p->tp_y[s] = e->value;
                ch = TRUE;
            } else if (e->type == EV_KEY && e->code == BTN_LEFT) { p->tp_click = e->value != 0; ch = TRUE; }
        }
    }
    if (ch) mark(p);
    return G_SOURCE_CONTINUE;
}

static void tp_attach(Pad *p, const char *path, int fd) {
    tp_close(p);
    p->tp_path = g_strdup(path);
    p->tp_fd = fd;
    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &p->tp_ax);
    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &p->tp_ay);
    p->tp_slot = 0;
    p->tp_watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_tp, p);
    mark(p);
}

/* Осі датчика руху. Спільна система (як у SDL): x — праворуч, y — вгору (з лицевого боку),
 * z — до гравця; гіроскоп: тангаж (навколо x), поворот (навколо y), крен (навколо z).
 * Sony (hid-playstation) і Switch 1 (hid-nintendo) уже так віддають. Міст Switch 2 — сирі осі
 * датчика геймпада (лежить рівно: z = +1 g) — переставляємо. */
static void imu_axes(Pad *p) {
    static const int ID_I[3] = { 0, 1, 2 }, ID_S[3] = { 1, 1, 1 }, GY_S[3] = { 1, 1, -1 };   /* крен SDL — навпаки */
    memcpy(p->acc_i, ID_I, sizeof ID_I); memcpy(p->acc_s, ID_S, sizeof ID_S);
    memcpy(p->gyr_i, ID_I, sizeof ID_I); memcpy(p->gyr_s, GY_S, sizeof GY_S);
    if (g_str_has_prefix(p->key, "ngc/")) {
        /* сирі осі Switch 2 (перевірено записом рухів): x — праворуч, y — до верху (від себе), z — вгору */
        static const int AI[3] = { 0, 2, 1 }, AS[3] = { 1, 1, -1 };    /* праворуч = x, вгору = z, до себе = −y */
        static const int GI[3] = { 0, 2, 1 }, GS[3] = { 1, 1, 1 };     /* вперед = gx, поворот = gz, вбік = gy */
        memcpy(p->acc_i, AI, sizeof AI); memcpy(p->acc_s, AS, sizeof AS);
        memcpy(p->gyr_i, GI, sizeof GI); memcpy(p->gyr_s, GS, sizeof GS);
    }
}

/* ═════════════ ЗАДНІ КНОПКИ (міст Switch 2) ═════════════
 * Міст читає розкладку з ~/.config/nso-gc/config.json → "button_map" (назва кнопки Switch →
 * код evdev) і підхоплює зміни сам (~1 с, без перепідключення; локальна правка мосту). */
/* Код evdev → назва для конфігу мосту і підпис */
static const struct { int code; const char *name, *label; } CODES[] = {
    { BTN_SOUTH, "BTN_SOUTH", "A" }, { BTN_EAST, "BTN_EAST", "B" }, { BTN_WEST, "BTN_WEST", "X" }, { BTN_NORTH, "BTN_NORTH", "Y" },
    { BTN_TL, "BTN_TL", "L" }, { BTN_TR, "BTN_TR", "R" }, { BTN_TL2, "BTN_TL2", "ZL" }, { BTN_TR2, "BTN_TR2", "ZR" },
    { BTN_START, "BTN_START", "+" }, { BTN_SELECT, "BTN_SELECT", "−" }, { BTN_MODE, "BTN_MODE", "Home" },
    { BTN_Z, "BTN_Z", "Знімок" }, { BTN_C, "BTN_C", "C" }, { BTN_THUMBL, "BTN_THUMBL", "Лів. стік" },
    { BTN_THUMBR, "BTN_THUMBR", "Прав. стік" },
};

static const char *DEFAULT_MAP[][2] = {          /* як PRO_BUTTON_MAP у мосту */
    { "A", "BTN_SOUTH" }, { "B", "BTN_EAST" }, { "X", "BTN_WEST" }, { "Y", "BTN_NORTH" }, { "L", "BTN_TL" },
    { "R", "BTN_TR" }, { "ZL", "BTN_TL2" }, { "ZR", "BTN_TR2" }, { "PLUS", "BTN_START" }, { "MINUS", "BTN_SELECT" },
    { "HOME", "BTN_MODE" }, { "CAPTURE", "BTN_Z" }, { "C", "BTN_C" }, { "L_STK", "BTN_THUMBL" }, { "R_STK", "BTN_THUMBR" },
    { "GL", "BTN_GRIPL" }, { "GR", "BTN_GRIPR" },
};
static const char *OWN[2] = { "BTN_GRIPL", "BTN_GRIPR" }, *BACK_NAME[2] = { "GL", "GR" };

static char *cfg_path(void) { return g_build_filename(g_get_user_config_dir(), "nso-gc", "config.json", NULL); }

static void read_back(Pad *p) {
    p->back[0] = p->back[1] = NULL;
    if (!g_str_has_prefix(p->key, "ngc/")) return;
    char *f = cfg_path();
    JsonParser *jp = json_parser_new();
    if (json_parser_load_from_file(jp, f, NULL)) {
        JsonObject *o = json_node_get_object(json_parser_get_root(jp));
        JsonObject *m = json_object_has_member(o, "button_map") ? json_object_get_object_member(o, "button_map") : NULL;
        for (int s = 0; s < 2; s++) {
            const char *c = m && json_object_has_member(m, BACK_NAME[s]) ? json_object_get_string_member(m, BACK_NAME[s])
                          : (m && json_object_get_size(m) > 0 ? "" : NULL);   /* розкладка є, а кнопки нема — вимкнено */
            p->back[s] = NULL;
            if (c && !*c) p->back[s] = "вимк.";
            for (guint i = 0; c && i < G_N_ELEMENTS(CODES); i++) if (!g_strcmp0(CODES[i].name, c)) p->back[s] = CODES[i].label;
        }
    }
    g_object_unref(jp); g_free(f);
}

static void write_back(int side, const char *code) {
    char *f = cfg_path();
    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_file(jp, f, NULL)) { g_object_unref(jp); g_free(f); return; }
    JsonNode *root = json_node_copy(json_parser_get_root(jp));
    JsonObject *o = json_node_get_object(root);
    if (!json_object_has_member(o, "button_map") || json_object_get_size(json_object_get_object_member(o, "button_map")) == 0) {
        JsonObject *m = json_object_new();         /* порожня = стандартна → записуємо повну */
        for (guint i = 0; i < G_N_ELEMENTS(DEFAULT_MAP); i++) json_object_set_string_member(m, DEFAULT_MAP[i][0], DEFAULT_MAP[i][1]);
        json_object_set_object_member(o, "button_map", m);
    }
    JsonObject *m = json_object_get_object_member(o, "button_map");
    if (code && !*code) json_object_remove_member(m, BACK_NAME[side]);            /* вимкнено */
    else json_object_set_string_member(m, BACK_NAME[side], code ? code : OWN[side]);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, TRUE);
    gchar *txt = json_generator_to_data(gen, NULL);
    g_file_set_contents(f, txt, -1, NULL);       /* атомарно (через тимчасовий файл) */
    g_free(txt); g_object_unref(gen); json_node_unref(root); g_object_unref(jp); g_free(f);
}

static void learn_label(Pad *p, int side) {
    if (!p->learn_btn[side]) return;
    char *t = p->learn == side + 1 ? g_strdup("Натисни кнопку…")
                                   : g_strdup_printf("%s: %s", BACK_NAME[side], p->back[side] ? p->back[side] : "своя");
    gtk_button_set_label(GTK_BUTTON(p->learn_btn[side]), t);
    g_free(t);
}

static gboolean learn_timeout(gpointer d) {      /* 10 с нічого не натиснули — скасувати */
    Pad *p = d;
    int side = p->learn - 1;
    p->learn = 0; p->learn_src = 0;
    if (side >= 0) learn_label(p, side);
    return G_SOURCE_REMOVE;
}

/* Натиснули кнопку на геймпаді в режимі «призначити» */
static void learn_key(Pad *p, int code) {
    int side = p->learn - 1;
    if (p->learn_src) { g_source_remove(p->learn_src); p->learn_src = 0; }
    p->learn = 0;
    if (code == BTN_GRIPL || code == BTN_GRIPR) {             /* сама задня — повернути «свою» */
        write_back(side, NULL);
        p->back[side] = NULL;
    } else {
        for (guint i = 0; i < G_N_ELEMENTS(CODES); i++)
            if (CODES[i].code == code) { write_back(side, CODES[i].name); p->back[side] = CODES[i].label; }
    }
    learn_label(p, side);
    mark(p);
}

static void on_learn(GtkButton *b, Pad *p) {
    int side = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "side"));
    if (p->learn_src) { g_source_remove(p->learn_src); p->learn_src = 0; }
    int was = p->learn;
    p->learn = was == side + 1 ? 0 : side + 1;   /* повторний клік — скасувати */
    learn_label(p, 0); learn_label(p, 1);
    if (p->learn) p->learn_src = g_timeout_add_seconds(10, learn_timeout, p);
}

/* ═════════════ ЗАРЯД (якщо драйвер його показує) ═════════════ */
static void read_battery(Pad *p) {
    p->battery = -1;
    if (!p->mac) return;
    if (g_str_has_prefix(p->key, "ngc/")) {      /* міст Switch 2 пише заряд у ~/.config/nso-gc/state.json */
        char *f = g_build_filename(g_get_user_config_dir(), "nso-gc", "state.json", NULL);
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_file(jp, f, NULL)) {
            JsonObject *o = json_node_get_object(json_parser_get_root(jp));
            JsonArray *a = json_object_has_member(o, "controllers") ? json_object_get_array_member(o, "controllers") : NULL;
            for (guint i = 0; a && i < json_array_get_length(a); i++) {
                JsonObject *c = json_array_get_object_element(a, i);
                if (!g_ascii_strcasecmp(json_object_get_string_member_with_default(c, "mac", ""), p->mac) &&
                    json_object_has_member(c, "battery_pct") && !json_object_get_null_member(c, "battery_pct"))
                    p->battery = json_object_get_int_member(c, "battery_pct");
            }
        }
        g_object_unref(jp); g_free(f);
        return;
    }
    char *mac = g_ascii_strdown(p->mac, -1);
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name(d))) {
        char *low = g_ascii_strdown(n, -1);
        if (strstr(low, mac)) {
            char *f = g_build_filename("/sys/class/power_supply", n, "capacity", NULL), *s = NULL;
            if (g_file_get_contents(f, &s, NULL, NULL)) p->battery = atoi(s);
            g_free(s); g_free(f);
        }
        g_free(low);
    }
    if (d) g_dir_close(d);
    g_free(mac);
}

static void fill_header(Pad *p) {
    char *t = g_strdup_printf("Контролер %d", p->slot);
    gtk_label_set_text(GTK_LABEL(p->num), t);
    g_free(t);
    gtk_label_set_text(GTK_LABEL(p->name_l), friendly(p));
    gtk_widget_set_tooltip_text(p->name_l, p->name);
    gtk_label_set_text(GTK_LABEL(p->conn), p->bus == BUS_BLUETOOTH ? "\U000F00AF" : "\U000F0553");   /* 󰂯 / 󰕓 */
    gtk_widget_set_tooltip_text(p->conn, p->bus == BUS_BLUETOOTH ? "Bluetooth" : "Кабель (USB)");
    if (p->battery >= 0) {
        static const char *B[] = { "\U000F007A", "\U000F007B", "\U000F007C", "\U000F007D", "\U000F007E",
                                   "\U000F007F", "\U000F0080", "\U000F0081", "\U000F0082", "\U000F0079" };
        char *b = g_strdup_printf("%s %d%%", B[CLAMP(p->battery / 10 - (p->battery == 100), 0, 9)], p->battery);
        gtk_label_set_text(GTK_LABEL(p->bat), b);
        g_free(b);
    } else gtk_label_set_text(GTK_LABEL(p->bat), "");
}

static gboolean battery_tick(gpointer d) {
    for (guint i = 0; i < pads->len; i++) { Pad *p = pads->pdata[i]; read_battery(p); if (p->box) fill_header(p); }
    return G_SOURCE_CONTINUE;
}

/* Раз на секунду — число затримки в куті картки (не щокадру: зайва перекладка тексту) */
static gboolean lat_tick(gpointer d) {
    for (guint i = 0; i < pads->len; i++) {
        Pad *p = pads->pdata[i];
        if (!p->lat) continue;
        char t[24];
        if (p->lat_ms > 0) g_snprintf(t, sizeof t, "%.0f мс", p->lat_ms); else g_snprintf(t, sizeof t, "— мс");
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(p->lat)), t)) gtk_label_set_text(GTK_LABEL(p->lat), t);
    }
    return G_SOURCE_CONTINUE;
}

/* ═════════════ ВІДКЛЮЧЕННЯ ═════════════ */
static void on_disconnect(GtkButton *b, Pad *p) {
    if (!p->mac) return;
    char *argv[] = { "bluetoothctl", "disconnect", p->mac, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    gtk_button_set_label(b, "Відключаю…");
}

/* ═════════════ ЧИ НА ЗВ'ЯЗКУ (міст Switch 2) ═════════════
 * Звичайні геймпади при відключенні просто зникають з /dev/input. Міст Switch 2 (phys «ngc/…»)
 * свій віртуальний геймпад лишає (щоб гра не втратила його до повторного підключення) —
 * тож для нього питаємо BlueZ: Device1.Connected, і слухаємо його зміни. */
static gboolean bridged(Pad *p) { return p->mac && g_str_has_prefix(p->key, "ngc/"); }

static char *dev_path(const char *mac) {
    char *m = g_strdup(mac);
    g_strdelimit(m, ":", '_');
    char *path = g_strdup_printf("/org/bluez/hci0/dev_%s", m);
    g_free(m);
    return path;
}

static gboolean bluez_linked(Pad *p) {
    if (!bridged(p) || g_getenv("DESK_TEST_OVERLAY")) return TRUE;   /* тестова копія — показувати все */
    if (!sysbus) return TRUE;                    /* без шини — не ховаємо */
    char *path = dev_path(p->mac);
    GVariant *r = g_dbus_connection_call_sync(sysbus, "org.bluez", path, "org.freedesktop.DBus.Properties", "Get",
                                              g_variant_new("(ss)", "org.bluez.Device1", "Connected"), G_VARIANT_TYPE("(v)"),
                                              G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    g_free(path);
    gboolean on = FALSE;
    if (r) { GVariant *v; g_variant_get(r, "(v)", &v); on = g_variant_get_boolean(v); g_variant_unref(v); g_variant_unref(r); }
    return on;
}

static void set_linked_by_path(const char *path, gboolean on) {
    for (guint i = 0; i < pads->len; i++) {
        Pad *p = pads->pdata[i];
        if (!bridged(p)) continue;
        char *mine = dev_path(p->mac);
        if (!g_strcmp0(mine, path) && p->linked != on) { p->linked = on; rebuild(); }
        g_free(mine);
    }
}

static void on_bluez_props(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                           const char *sig, GVariant *params, gpointer d) {
    const char *which;
    GVariant *changed;
    g_variant_get(params, "(&s@a{sv}@as)", &which, &changed, NULL);
    gboolean on;
    if (!g_strcmp0(which, "org.bluez.Device1") && g_variant_lookup(changed, "Connected", "b", &on))
        set_linked_by_path(path, on);
    g_variant_unref(changed);
}
static void on_bluez_removed(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                             const char *sig, GVariant *params, gpointer d) {
    const char *obj;
    g_variant_get(params, "(&oas)", &obj, NULL);
    set_linked_by_path(obj, FALSE);              /* пристрій забули — точно не на зв'язку */
}

/* Міст перед підключенням «забуває» пристрій у BlueZ, і той з'являється знову вже підключеним
 * (InterfacesAdded) — а не через PropertiesChanged */
static void on_bluez_added(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                           const char *sig, GVariant *params, gpointer d) {
    const char *obj;
    GVariant *ifaces, *dev;
    g_variant_get(params, "(&o@a{sa{sv}})", &obj, &ifaces);
    gboolean on;
    if ((dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}")))) {
        if (g_variant_lookup(dev, "Connected", "b", &on)) set_linked_by_path(obj, on);
        g_variant_unref(dev);
    }
    g_variant_unref(ifaces);
}

/* ═════════════ СПИСОК НА КАРТЦІ ═════════════ */
static void build_section(Pad *p) {
    p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->num = gtk_label_new(""); add_class(p->num, "pad-num");
    p->bat = gtk_label_new(""); add_class(p->bat, "pad-bat");
    p->conn = gtk_label_new(""); add_class(p->conn, "pad-conn");
    p->lat = gtk_label_new("— мс"); add_class(p->lat, "pad-lat");   /* затримка — правий верхній кут */
    gtk_widget_set_tooltip_text(p->lat, p->imu_path ? "Як часто приходять дані руху (період звітів)"
                                                    : "Скільки подія йде від драйвера до віджета");
    gtk_box_pack_start(GTK_BOX(head), p->num, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(head), p->lat, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(head), p->conn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(head), p->bat, FALSE, FALSE, 0);
    p->name_l = gtk_label_new("");
    add_class(p->name_l, "pad-name");
    gtk_label_set_xalign(GTK_LABEL(p->name_l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p->name_l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(p->name_l), 1);
    p->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(p->area, AREA_W, PAD_PX + (p->imu_path ? GYRO_H : 0));
    gtk_widget_set_halign(p->area, GTK_ALIGN_CENTER);
    g_signal_connect(p->area, "draw", G_CALLBACK(on_draw), p);
    gboolean bt = p->bus == BUS_BLUETOOTH && p->mac;
    p->btn = gtk_button_new_with_label(bt ? "\U000F00B2  Відключити" : "\U000F0553  Підключено кабелем");   /* 󰂲 / 󰕓 */
    add_class(p->btn, "pad-off");
    gtk_widget_set_sensitive(p->btn, bt);
    gtk_widget_set_can_focus(p->btn, FALSE);
    g_signal_connect(p->btn, "clicked", G_CALLBACK(on_disconnect), p);
    GtkWidget *backs = NULL;                     /* задні кнопки — лише Pro 2 через міст (кабельний драйвер їх не перепризначає) */
    p->learn_btn[0] = p->learn_btn[1] = NULL;
    if (p->type == T_PRO2 && g_str_has_prefix(p->key, "ngc/")) {
        backs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        add_class(backs, "pad-backs");
        gtk_box_set_homogeneous(GTK_BOX(backs), TRUE);
        for (int s = 0; s < 2; s++) {
            GtkWidget *b = gtk_button_new_with_label("");
            add_class(b, "pad-learn");
            gtk_widget_set_can_focus(b, FALSE);
            gtk_widget_set_tooltip_text(b, "Клік — потім натисни на геймпаді кнопку, яку ставимо на цю задню.\n"
                                           "Натиснути саму задню — повернути «свою». Ще клік — скасувати.");
            g_object_set_data(G_OBJECT(b), "side", GINT_TO_POINTER(s));
            g_signal_connect(b, "clicked", G_CALLBACK(on_learn), p);
            p->learn_btn[s] = b;
            gtk_box_pack_start(GTK_BOX(backs), b, TRUE, TRUE, 0);
            learn_label(p, s);
        }
    }
    for (GtkWidget **w = (GtkWidget *[]){ head, p->name_l, p->area, backs ? backs : p->btn, backs ? p->btn : NULL, NULL }; *w; w++)
        gtk_box_pack_start(GTK_BOX(p->box), *w, FALSE, FALSE, 0);
    if (p->bus == BUS_USB && !g_str_has_prefix(p->key, "ngc/"))   /* на кабелі — сполучити по Bluetooth */
        gtk_box_pack_start(GTK_BOX(p->box), btlink_new(p->name, p->mac, "gaming"), FALSE, FALSE, 0);
    fill_header(p);
}

/* ═════════════ КАРТКИ: кожен геймпад — окреме вікно ═════════════
 * «Контролер N» — за черговістю підключення серед тих, що зараз на зв'язку: перший — 1,
 * наступний — 2… Відключився один — решта зсуваються вгору без дірок (2 → 1, 3 → 2…),
 * і картка переїжджає на місце свого нового номера.
 * Позиція кожного номера — ~/.cache/deskpadN.pos (за замовчуванням — рядком ліворуч). */
static gboolean pads_on(void) {
    char *f = g_build_filename(g_get_home_dir(), ".config/hypr/pads.off", NULL);
    gboolean on = !g_file_test(f, G_FILE_TEST_EXISTS);
    g_free(f);
    return on;
}

static void card_free(Pad *p) {
    if (!p->card) return;
    cards_unregister(p->card);
    gtk_widget_destroy(p->card->win);
    g_free((char *)p->card->pos_name);
    g_free(p->card);
    p->card = NULL;
    p->box = p->area = p->lat = NULL;
    p->learn_btn[0] = p->learn_btn[1] = NULL;
    p->slot = 0;
}

static gint by_seq(gconstpointer a, gconstpointer b) {
    const Pad *x = *(Pad *const *)a, *y = *(Pad *const *)b;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static gboolean recheck(gpointer d);
static void recheck_soon(void) { g_timeout_add(120, recheck, NULL); }

/* Кабельний режим моста захоплює пристрій ядра і дає свій (phys «ngc/USB-<серійник>»):
 * пристрій ядра з тим самим серійником (uniq) — тінь, не показуємо */
static gboolean shadowed(Pad *p) {
    if (g_str_has_prefix(p->key, "ngc/")) return FALSE;
    for (guint i = 0; i < pads->len; i++) {
        Pad *q = pads->pdata[i];
        if (g_str_has_prefix(q->key, "ngc/usb-") || g_str_has_prefix(q->key, "ngc/USB-"))
            if (!g_ascii_strcasecmp(q->key + 8, p->key)) return TRUE;
    }
    return FALSE;
}

static gboolean do_rebuild(gpointer d) {
    rebuild_src = 0;
    gboolean on = pads_on();
    for (guint i = 0; i < pads->len; i++) {      /* тінь не показуємо */
        Pad *p = pads->pdata[i];
        if (shadowed(p)) p->linked = FALSE; else if (!bridged(p)) p->linked = TRUE;
    }
    for (guint i = 0; i < pads->len; i++) {      /* спершу прибрати непотрібні — звільняться номери */
        Pad *p = pads->pdata[i];
        if (!(on && p->linked)) { card_free(p); p->seq = 0; }
    }
    static guint seq_next;
    GPtrArray *shown = g_ptr_array_new();
    for (guint i = 0; i < pads->len; i++) {
        Pad *p = pads->pdata[i];
        if (!(on && p->linked)) continue;
        if (!p->seq) p->seq = ++seq_next;      /* щойно з'явився — у кінець черги */
        g_ptr_array_add(shown, p);
    }
    g_ptr_array_sort(shown, by_seq);
    for (guint i = 0; i < shown->len; i++) {    /* номер змінився (хтось перед ним відключився) — картку на нове місце */
        Pad *p = shown->pdata[i];
        if (p->card && p->slot != (int)i + 1) card_free(p);
    }
    for (guint i = 0; i < shown->len; i++) {    /* у порядку підключення */
        Pad *p = shown->pdata[i];
        if (!p->card) {
            p->slot = i + 1;
            Card *c = p->card = g_new0(Card, 1);
            c->pos_name = g_strdup_printf("deskpad%d.pos", p->slot);
            c->off_name = "pads.off";
            if (!load_pos(c)) { c->mright = 420 + (p->slot - 1) * (CARD_W + GRID); c->mtop = 100; }
            c->moved = recheck_soon;
            card_init(c, "desktop-pads", "pads");
            p->vis = TRUE;
            g_timeout_add(300, recheck, NULL);
        } else {
            GList *kids = gtk_container_get_children(GTK_CONTAINER(p->card->card));
            for (GList *l = kids; l; l = l->next) gtk_widget_destroy(l->data);
            g_list_free(kids);
        }
        build_section(p);
        gtk_box_pack_start(GTK_BOX(p->card->card), p->box, FALSE, FALSE, 0);
        gtk_widget_show_all(p->card->win);
        gtk_window_resize(GTK_WINDOW(p->card->win), 1, 1);   /* зникла смуга гіроскопа — картка менша */
        mark(p);
    }
    devconn_online("gaming", shown->len > 0);    /* є живі картки — «підключити контролер» сховати */
    g_ptr_array_unref(shown);
    return G_SOURCE_REMOVE;
}
static void rebuild(void) {
    if (!rebuild_src) rebuild_src = g_idle_add(do_rebuild, NULL);
}
void pads_apply(void) { if (pads) rebuild(); }   /* SIGHUP (desk.sh toggle pads) */

/* ═════════════ ПОЯВА / ЗНИКНЕННЯ ПРИСТРОЇВ ═════════════ */
static void free_pad(Pad *p) {
    if (p->watch) g_source_remove(p->watch);
    if (p->learn_src) g_source_remove(p->learn_src);
    imu_close(p);
    tp_close(p);
    if (p->fd >= 0) close(p->fd);
    g_free(p->path); g_free(p->name); g_free(p->key); g_free(p->mac);
    g_free(p);
}

static void remove_pad(Pad *p) {
    /* датчик руху / тачпад ще живі — чекають свого геймпада */
    if (p->imu_path) g_ptr_array_add(orphans, g_strdup(p->imu_path));
    if (p->tp_path) g_ptr_array_add(orphans, g_strdup(p->tp_path));
    if (p->bus == BUS_USB && !g_str_has_prefix(p->key, "ngc/")) btlink_unplugged(p->name, p->mac);   /* кабель витягнули — по Bluetooth */
    g_ptr_array_remove(pads, p);
    card_free(p);                                /* одразу: малюнок не має звертатися до звільненого */
    free_pad(p);
    rebuild();
}


static Pad *find_by_key(const char *k) {
    for (guint i = 0; i < pads->len; i++) { Pad *p = pads->pdata[i]; if (!g_strcmp0(p->key, k)) return p; }
    return NULL;
}

/* Що за пристрій: 0 — інше, 1 — геймпад, 2 — датчик руху, 3 — тачпад геймпада */
static int classify(int fd) {
    unsigned long evb[NBITS(EV_CNT)] = { 0 }, keyb[NBITS(KEY_CNT)] = { 0 }, absb[NBITS(ABS_CNT)] = { 0 }, props[NBITS(INPUT_PROP_CNT)] = { 0 };
    ioctl(fd, EVIOCGBIT(0, sizeof evb), evb);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), keyb);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
    ioctl(fd, EVIOCGPROP(sizeof props), props);
    if (bit(props, INPUT_PROP_ACCELEROMETER) && bit(absb, ABS_X)) return 2;
    if (bit(keyb, BTN_TOUCH) && bit(absb, ABS_MT_POSITION_X) && !bit(keyb, BTN_SOUTH)) return 3;
    if (bit(evb, EV_ABS) && !bit(evb, EV_REL) && !bit(keyb, BTN_TOUCH) && (bit(absb, ABS_X) || bit(absb, ABS_RX)) &&
        (bit(keyb, BTN_SOUTH) || bit(keyb, BTN_DPAD_UP) || bit(keyb, BTN_TR) || bit(keyb, BTN_TL))) return 1;
    return 0;
}

/* Допоміжний пристрій (рух / тачпад) — до геймпада з тим самим ключем. TRUE — прийнято (fd забрано) */
static gboolean attach_extra(int kind, const char *path, int fd, const char *k) {
    Pad *p = find_by_key(k);
    if (!p) return FALSE;
    if (kind == 2) imu_attach(p, path, fd);
    else tp_attach(p, path, fd);
    return TRUE;
}

/* 1 — додано / вже відомий, 0 — не наш, −1 — поки нема доступу (спробувати пізніше) */
static int try_device(const char *path) {
    for (guint i = 0; i < pads->len; i++) {
        Pad *p = pads->pdata[i];
        if (!g_strcmp0(p->path, path) || !g_strcmp0(p->imu_path, path) || !g_strcmp0(p->tp_path, path)) return 1;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == EACCES || errno == EPERM ? -1 : 0;
    int kind = classify(fd);
    char *uniq = get_str(fd, 2), *phys = get_str(fd, 1), *k = dev_key(uniq, phys);
    int res = 0;
    if (kind == 2 || kind == 3) {
        if (!attach_extra(kind, path, fd, k)) {
            gboolean known = FALSE;
            for (guint i = 0; i < orphans->len; i++) if (!g_strcmp0(orphans->pdata[i], path)) known = TRUE;
            if (!known) g_ptr_array_add(orphans, g_strdup(path));
            close(fd);
        }
        res = 1;
    } else if (kind == 1) {
        unsigned long absb[NBITS(ABS_CNT)] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
        Pad *p = g_new0(Pad, 1);
        struct input_id id = { 0 };
        ioctl(fd, EVIOCGID, &id);
        p->fd = fd; p->imu_fd = -1; p->tp_fd = -1;
        p->tp_id[0] = p->tp_id[1] = -1;
        p->path = g_strdup(path);
        p->name = get_str(fd, 0);
        p->key = k; k = NULL;
        p->bus = id.bustype; p->vendor = id.vendor; p->product = id.product;
        if (is_mac(uniq)) p->mac = g_ascii_strup(uniq, -1);
        else if (g_str_has_prefix(phys, "ngc/") && is_mac(phys + 4)) p->mac = g_ascii_strup(phys + 4, -1);   /* Bluetooth-міст Switch 2 */
        if (p->mac && p->bus != BUS_BLUETOOTH && g_str_has_prefix(phys, "ngc/")) p->bus = BUS_BLUETOOTH;
        if (g_str_has_prefix(phys, "ngc/USB-")) p->bus = BUS_USB;   /* кабельний режим моста */
        for (int c = 0; c < ABS_CNT; c++) p->has[c] = bit(absb, c);
        p->type = detect(p);
        p->linked = bluez_linked(p);
        imu_axes(p);
        read_back(p);
        face_map(p);
        sync_state(p);
        read_battery(p);
        int clk = CLOCK_MONOTONIC;
        ioctl(fd, EVIOCSCLOCKID, &clk);          /* час подій — монотонний (для затримки) */
        p->watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_pad, p);
        g_ptr_array_add(pads, p);
        for (guint i = 0; i < orphans->len;) {   /* рух / тачпад з'явились раніше за геймпад */
            const char *op = orphans->pdata[i];
            int ofd = open(op, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            gboolean taken = FALSE;
            if (ofd >= 0) {
                char *ou = get_str(ofd, 2), *oph = get_str(ofd, 1), *ok = dev_key(ou, oph);
                int ok_kind = classify(ofd);
                if (!g_strcmp0(ok, p->key) && (ok_kind == 2 || ok_kind == 3)) taken = attach_extra(ok_kind, op, ofd, ok);
                if (!taken) close(ofd);
                g_free(ou); g_free(oph); g_free(ok);
            }
            if (taken) g_ptr_array_remove_index(orphans, i); else i++;
        }
        rebuild();
        res = 1;
    } else close(fd);
    g_free(uniq); g_free(phys); g_free(k);
    return res;
}

typedef struct { char *path; int tries; } Pending;
static gboolean retry_device(gpointer d) {       /* щойно з'явився: udev ще може ставити права */
    Pending *pd = d;
    if (try_device(pd->path) == -1 && ++pd->tries < 8) return G_SOURCE_CONTINUE;
    g_free(pd->path); g_free(pd);
    return G_SOURCE_REMOVE;
}

static void on_dir(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer d) {
    char *path = g_file_get_path(f);
    char *base = g_path_get_basename(path);
    if (g_str_has_prefix(base, "event")) {
        if (ev == G_FILE_MONITOR_EVENT_CREATED || ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) {
            Pending *pd = g_new0(Pending, 1);
            pd->path = g_strdup(path);
            g_timeout_add(250, retry_device, pd);
        } else if (ev == G_FILE_MONITOR_EVENT_DELETED) {
            for (guint i = 0; i < orphans->len; i++)
                if (!g_strcmp0(orphans->pdata[i], path)) { g_ptr_array_remove_index(orphans, i); break; }
        }
    }
    g_free(base); g_free(path);
}

/* ═════════════ ВИДИМІСТЬ (картку закрило вікно — не малюємо) ═════════════ */
static HwRect pad_rect(HwRect m, gpointer d) {
    Card *c = d;
    double w = gtk_widget_get_allocated_width(c->win), h = gtk_widget_get_allocated_height(c->win);
    return (HwRect){ m.x + m.w - c->mright - w, m.y + c->mtop, w, h };
}
static gboolean recheck(gpointer d) {
    for (guint i = 0; i < pads->len; i++) {
        Pad *p = pads->pdata[i];
        if (!p->card) continue;
        gboolean v = g_getenv("DESK_TEST_OVERLAY") ? TRUE : hw_visible(pad_rect, p->card);
        if (v != p->vis) { p->vis = v; if (v) mark(p); }
    }
    return G_SOURCE_REMOVE;
}

void pads_init(void) {
    sysbus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (sysbus) {
        g_dbus_connection_signal_subscribe(sysbus, "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                           NULL, "org.bluez.Device1", G_DBUS_SIGNAL_FLAGS_NONE, on_bluez_props, NULL, NULL);
        g_dbus_connection_signal_subscribe(sysbus, "org.bluez", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                                           "/", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_bluez_removed, NULL, NULL);
        g_dbus_connection_signal_subscribe(sysbus, "org.bluez", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                                           "/", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_bluez_added, NULL, NULL);
    }
    pads = g_ptr_array_new();
    orphans = g_ptr_array_new_with_free_func(g_free);
    GDir *dir = g_dir_open("/dev/input", 0, NULL);
    const char *n;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    while (dir && (n = g_dir_read_name(dir)))
        if (g_str_has_prefix(n, "event")) g_ptr_array_add(names, g_build_filename("/dev/input", n, NULL));
    if (dir) g_dir_close(dir);
    for (int pass = 0; pass < 2; pass++)         /* 2 проходи: датчики руху, знайдені раніше за геймпад */
        for (guint i = 0; i < names->len; i++) try_device(names->pdata[i]);
    g_ptr_array_unref(names);

    GFile *f = g_file_new_for_path("/dev/input");
    GFileMonitor *mon = g_file_monitor_directory(f, G_FILE_MONITOR_NONE, NULL, NULL);
    if (mon) g_signal_connect(mon, "changed", G_CALLBACK(on_dir), NULL);
    g_object_unref(f);
    g_timeout_add_seconds(30, battery_tick, NULL);
    g_timeout_add_seconds(1, lat_tick, NULL);
    hw_listen(recheck);
}
