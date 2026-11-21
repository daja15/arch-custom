/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  STARS — зірки «-|-», що спалахують по всьому екрану         │
 * │  (C + GTK3, layer-shell)                                     │
 * │                                                              │
 * │  У випадковому місці екрана зірка з ASCII-символів           │
 * │  «розгоряється» й гасне:                                     │
 * │        .   →   +   →   -|-   →   +   →   .                   │
 * │  Три розміри, колір — з теми (здебільшого «bright»: світла   │
 * │  тема → темні зірки, темна → світлі; зрідка акценти).        │
 * │                                                              │
 * │  Частота: STARS_PER_SEC ≈ 2% від того, скільки крапель за    │
 * │  секунду з'являлось у дощі (~530/с при силі 100%) → ~10/с.   │
 * │                                                              │
 * │  Економія:                                                   │
 * │   • символи намальовані ОДИН раз (спрайти) — щокадру лише    │
 * │     копіюються з потрібною прозорістю;                       │
 * │   • Hyprland отримує лише маленькі прямокутники зірок, що    │
 * │     змінились (їх одночасно ~20), а не весь екран;           │
 * │   • 12 к/с на спільній «сітці кадрів» (hyprwatch); новий     │
 * │     кадр — лише коли екран показав попередній;               │
 * │   • пауза: повноекранне вікно, режим «Енергозбереження».     │
 * │                                                              │
 * │  Вмикати / вимикати: desk.sh toggle stars (або меню % у rofi)│
 * │  Збірка — ~/.config/hypr/src/build.sh stars                  │
 * │  Перевірка: STARS_TEST_OVERLAY=1 (поверх усього, без пауз),  │
 * │  STARS_DEBUG=1 (кадри/с і кількість зірок раз на секунду).   │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <gtk-layer-shell.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>
#include <sys/mman.h>
#include <unistd.h>
#include <math.h>
#include <string.h>

#include "hyprwatch.h"
#include "theme.h"

/* ═════════════ НАЛАШТУВАННЯ ═════════════ */
#define STARS_PER_SEC  10.0   /* скільки зірок спалахує за секунду (≈ 2% від колишнього дощу) */
/* Кожна зірка живе по-своєму: розгоряється (RISE), світить і пульсує (HOLD),
   гасне (FALL) — тривалість кожної фази випадкова між MIN і MAX, секунд. */
#define RISE_MIN       0.25
#define RISE_MAX       0.9
#define HOLD_MIN       0.2    /* коротке «блим» … */
#define HOLD_MAX       4.0    /* … або довге мерехтіння */
#define FALL_MIN       0.3
#define FALL_MAX       1.2
#define PULSE_MIN      0.35   /* період однієї пульсації, с (у кожної зірки свій) */
#define PULSE_MAX      1.6
#define STEADY_PROB    0.3    /* частка зірок, що світять рівно, без пульсації */
#define OVER_WINDOWS   1      /* 1 = поверх вікон (під waybar і rofi), 0 = лише на столі, під вікнами */
#define BRIGHTNESS     0.9    /* найбільша непрозорість зірки (0…1) */
#define FPS            12     /* кадрів/с. Замір 2026-11-15 (база: Hyprland 4%, GPU 6%):
                                 24 к/с → Hyprland 20%, GPU 19%; 12 к/с → 11% / 14%.
                                 Ціна — сам факт кадру в Hyprland, а не розмір зірок
                                 (один охопний прямокутник замість кількох — те саме).
                                 12 ділить 24 — на спільній сітці з еквалайзером */
#define EDGE           24     /* не ставити зірки ближче цього до країв екрана (px) */

/* Стадії спалаху (символи): . + -|-  — див. star_look() */
static const char *STAGE_TXT[] = { ".", "+", "-|-" };

/* Розміри шрифту (px) і як часто кожен трапляється */
static const double SIZE_PX[]   = { 12, 16, 22 };
static const double SIZE_PROB[] = { 0.50, 0.35, 0.15 };
/* Кольори: 0 — «bright» теми, 1 — акцент, 2 — другий акцент */
static const double COL_PROB[]  = { 0.70, 0.15, 0.15 };

/* ═════════════ СТАН ═════════════ */
#define NSIZE 3
#define NCOL 3
#define NSTAGE 3
#define MAX_STARS 96
typedef struct { cairo_surface_t *s; int w, h; } Sprite;
static Sprite spr[NCOL][NSIZE][NSTAGE];

typedef struct {
    int x, y, size, col;
    double age, life, bright;
    double rise, hold, fall;     /* тривалість фаз, с */
    double period, phase, depth; /* пульсація: період (с), зсув, глибина (0 = рівне світло) */
    gboolean alive;
} Star;
static Star stars[MAX_STARS];
static double spawn_acc;

static int W, H, scale = 1;
static gboolean saver = FALSE, fullscreen = FALSE, test_mode = FALSE, running = FALSE;
static gint64 last_us;
static HwTick anim;
static GtkWidget *win;

/* ═════════════ WAYLAND: власна підповерхня ═════════════ */
typedef struct { int x, y, w, h; } Rect;
typedef struct { struct wl_buffer *buf; void *data; cairo_surface_t *cs; gboolean busy; GArray *drawn; } ShmBuf;
#define NBUF 3
static ShmBuf bufs[NBUF];
static GArray *shown;                      /* прямокутники зірок, які зараз на екрані */
static struct wl_subcompositor *wl_subcomp;
static struct wl_shm *wl_shm_g;
static struct wl_surface *surf;
static struct wl_subsurface *sub;
static gboolean frame_pending = FALSE;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    if (strcmp(iface, wl_subcompositor_interface.name) == 0) wl_subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (strcmp(iface, wl_shm_interface.name) == 0)      wl_shm_g = wl_registry_bind(r, name, &wl_shm_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void buf_release(void *d, struct wl_buffer *b) { ((ShmBuf *)d)->busy = FALSE; }
static const struct wl_buffer_listener buf_listener = { buf_release };

static void frame_done(void *d, struct wl_callback *cb, uint32_t t) { wl_callback_destroy(cb); frame_pending = FALSE; }
static const struct wl_callback_listener frame_listener = { frame_done };

static gboolean shm_buf(ShmBuf *b, int w, int h) {
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("stars", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { if (fd >= 0) close(fd); return FALSE; }
    b->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);   /* memfd — нулі = прозоро */
    if (b->data == MAP_FAILED) { close(fd); return FALSE; }
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm_g, fd, size);
    b->buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->buf, &buf_listener, b);
    b->cs = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32, w, h, stride);
    cairo_surface_set_device_scale(b->cs, scale, scale);
    b->drawn = g_array_new(FALSE, FALSE, sizeof(Rect));
    b->busy = FALSE;
    return TRUE;
}

/* ═════════════ СПРАЙТИ: символи зірки, намальовані один раз ═════════════ */
static void make_sprite(Sprite *sp, const char *txt, double px, const double rgb[3]) {
    PangoFontDescription *fd = pango_font_description_from_string(TH_FONT_MONO " Bold");
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *tc = cairo_create(tmp);
    PangoLayout *lay = pango_cairo_create_layout(tc);
    pango_layout_set_font_description(lay, fd);
    pango_layout_set_text(lay, txt, -1);
    PangoAttrList *at = pango_attr_list_new();               /* без лігатур: JetBrains Mono злив би «-|» у «⊣» */
    pango_attr_list_insert(at, pango_attr_font_features_new("liga 0, calt 0, dlig 0"));
    pango_layout_set_attributes(lay, at);
    pango_attr_list_unref(at);
    PangoRectangle ink;
    pango_layout_get_pixel_extents(lay, &ink, NULL);
    sp->w = ink.width + 4; sp->h = ink.height + 4;
    sp->s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sp->w * scale, sp->h * scale);
    cairo_surface_set_device_scale(sp->s, scale, scale);
    cairo_t *cr = cairo_create(sp->s);
    cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
    cairo_move_to(cr, 2 - ink.x, 2 - ink.y);       /* центр саме ЧОРНИЛА символа — «.» не стрибає донизу */
    pango_cairo_update_layout(cr, lay);
    pango_cairo_show_layout(cr, lay);
    cairo_destroy(cr);
    g_object_unref(lay);
    cairo_destroy(tc);
    cairo_surface_destroy(tmp);
    pango_font_description_free(fd);
}

static void make_sprites(void) {
    const double cols[NCOL][3] = { { TH_BRIGHT_RGB }, { TH_ACCENT_RGB }, { TH_ACCENT2_RGB } };
    for (int c = 0; c < NCOL; c++)
        for (int z = 0; z < NSIZE; z++)
            for (int s = 0; s < NSTAGE; s++) make_sprite(&spr[c][z][s], STAGE_TXT[s], SIZE_PX[z], cols[c]);
}

/* ═════════════ ЗІРКИ ═════════════ */
static int pick(const double *prob, int n) {
    double r = g_random_double(), acc = 0;
    for (int i = 0; i < n; i++) if (r < (acc += prob[i])) return i;
    return n - 1;
}

static void spawn(void) {
    for (int i = 0; i < MAX_STARS; i++) {
        if (stars[i].alive) continue;
        Star *s = &stars[i];
        for (int tries = 0; tries < 6; tries++) {          /* не накладати на сусідню зірку */
            s->x = g_random_int_range(EDGE, W - EDGE);
            s->y = g_random_int_range(EDGE, H - EDGE);
            gboolean near = FALSE;
            for (int j = 0; j < MAX_STARS && !near; j++)
                near = j != i && stars[j].alive && abs(stars[j].x - s->x) < 40 && abs(stars[j].y - s->y) < 40;
            if (!near) break;
        }
        s->size = pick(SIZE_PROB, NSIZE);
        s->col = pick(COL_PROB, NCOL);
        s->age = 0;
        double h = g_random_double();                      /* частіше короткі, зрідка довгі */
        s->rise = g_random_double_range(RISE_MIN, RISE_MAX);
        s->hold = HOLD_MIN + (HOLD_MAX - HOLD_MIN) * h * h;
        s->fall = g_random_double_range(FALL_MIN, FALL_MAX);
        s->life = s->rise + s->hold + s->fall;
        s->period = g_random_double_range(PULSE_MIN, PULSE_MAX);
        s->phase = g_random_double_range(0, 2 * G_PI);
        s->depth = g_random_double() < STEADY_PROB ? 0 : g_random_double_range(0.4, 1.0);
        s->bright = BRIGHTNESS * g_random_double_range(0.55, 1.0);
        s->alive = TRUE;
        return;
    }
}

static void simulate(double dt) {
    spawn_acc += STARS_PER_SEC * dt;
    while (spawn_acc >= 1) {                               /* у середньому STARS_PER_SEC, але не рівним кроком */
        spawn_acc -= g_random_double_range(0.4, 1.6);
        spawn();
    }
    for (int i = 0; i < MAX_STARS; i++)
        if (stars[i].alive && (stars[i].age += dt) >= stars[i].life) stars[i].alive = FALSE;
}

/* Вигляд і яскравість зірки зараз: . → + → -|- (пульсує -|- ↔ +) → + → .
   Повертає стадію (індекс у STAGE_TXT), *alpha — множник непрозорості 0…1. */
static int star_look(const Star *s, double *alpha) {
    double t = s->age;
    if (t < s->rise) {                                     /* розгоряється */
        double q = t / s->rise;
        *alpha = fmin(1, q / 0.5);
        return q < 0.5 ? 0 : 1;
    }
    t -= s->rise;
    if (t < s->hold) {                                     /* світить і пульсує */
        double v = 0.5 + 0.5 * cos(2 * G_PI * t / s->period + s->phase);   /* 0…1 */
        double k = 1 - s->depth * (1 - v);                 /* depth 0 → завжди 1 */
        *alpha = 0.45 + 0.55 * k;
        return k > 0.6 ? 2 : 1;
    }
    t -= s->hold;                                          /* гасне */
    double q = fmin(1, t / s->fall);
    *alpha = fmin(1, (1 - q) / 0.5);
    return q < 0.5 ? 1 : 0;
}

/* ═════════════ КАДР ═════════════ */
static ShmBuf *free_buf(void) {
    for (int i = 0; i < NBUF; i++) if (bufs[i].buf && !bufs[i].busy) return &bufs[i];
    return NULL;
}

static void render(gboolean empty) {
    ShmBuf *b = free_buf();
    if (!b || !surf) return;

    /* стерти в цьому буфері те, що в ньому було намальовано (2–3 кадри тому) — прямо в пам'яті */
    cairo_surface_flush(b->cs);
    guint8 *px = b->data;
    int stride = cairo_image_surface_get_stride(b->cs);
    for (guint i = 0; i < b->drawn->len; i++) {
        Rect *r = &g_array_index(b->drawn, Rect, i);
        int x0 = MAX(r->x, 0) * scale, x1 = MIN(r->x + r->w, W) * scale;
        int y0 = MAX(r->y, 0) * scale, y1 = MIN(r->y + r->h, H) * scale;
        for (int y = y0; y < y1 && x1 > x0; y++) memset(px + y * stride + x0 * 4, 0, (x1 - x0) * 4);
    }
    cairo_surface_mark_dirty(b->cs);
    g_array_set_size(b->drawn, 0);

    if (!empty) {
        cairo_t *cr = cairo_create(b->cs);
        for (int i = 0; i < MAX_STARS; i++) {
            Star *s = &stars[i];
            if (!s->alive) continue;
            double k;
            int st = star_look(s, &k);
            double a = s->bright * k;
            if (a <= 0.01) continue;
            Sprite *sp = &spr[s->col][s->size][st];
            int x = s->x - sp->w / 2, y = s->y - sp->h / 2;
            cairo_set_source_surface(cr, sp->s, x, y);
            cairo_paint_with_alpha(cr, a);
            Rect r = { x, y, sp->w, sp->h };
            g_array_append_val(b->drawn, r);
        }
        cairo_destroy(cr);
        cairo_surface_flush(b->cs);
    }
    if (b->drawn->len == 0 && shown->len == 0) return;     /* і було пусто, і є пусто — нічого не надсилати */

    /* Hyprland оновлює лише прямокутники зірок: де були минулого кадру + де є тепер */
    wl_surface_attach(surf, b->buf, 0, 0);
    for (guint i = 0; i < shown->len; i++) {
        Rect *r = &g_array_index(shown, Rect, i);
        wl_surface_damage(surf, r->x, r->y, r->w, r->h);
    }
    for (guint i = 0; i < b->drawn->len; i++) {
        Rect *r = &g_array_index(b->drawn, Rect, i);
        wl_surface_damage(surf, r->x, r->y, r->w, r->h);
    }
    struct wl_callback *cb = wl_surface_frame(surf);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    frame_pending = TRUE;
    b->busy = TRUE;
    wl_surface_commit(surf);
    wl_display_flush(gdk_wayland_display_get_wl_display(gdk_display_get_default()));
    g_array_set_size(shown, 0);
    g_array_append_vals(shown, b->drawn->data, b->drawn->len);
}

static gboolean step(gpointer d) {
    gint64 now = g_get_monotonic_time();
    double dt = last_us ? (now - last_us) / 1e6 : 1.0 / FPS;
    last_us = now;
    simulate(fmin(dt, 0.1));
    if (!frame_pending) render(FALSE);        /* екран ще не показав попередній кадр — пропускаємо */

    if (g_getenv("STARS_DEBUG")) {
        static int frames; static gint64 since;
        frames++;
        if (now - since > G_USEC_PER_SEC) {
            int alive = 0;
            for (int i = 0; i < MAX_STARS; i++) alive += stars[i].alive;
            g_printerr("кадрів/с %d, зірок %d, прямокутників %u\n", frames, alive, shown->len);
            frames = 0; since = now;
        }
    }
    return G_SOURCE_CONTINUE;
}

static void apply_state(void) {
    gboolean run = surf && !saver && !(fullscreen && OVER_WINDOWS && !test_mode);
    if (run && !running) {
        running = TRUE;
        last_us = 0;
        hw_tick_start(&anim, FPS, step);
    } else if (!run && running) {
        running = FALSE;
        hw_tick_stop(&anim);
        for (int i = 0; i < MAX_STARS; i++) stars[i].alive = FALSE;
        frame_pending = FALSE;
        render(TRUE);                         /* прибрати зірки з екрана */
    }
}

/* ═════════════ ПАУЗИ: повноекранне вікно, енергозбереження ═════════════ */
static gboolean on_windows(gpointer d) {
    char *out = NULL;
    char *argv[] = { "hyprctl", "-j", "activeworkspace", NULL };
    fullscreen = FALSE;
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL)) {
        JsonNode *n = json_from_string(out, NULL);
        if (n && JSON_NODE_HOLDS_OBJECT(n))
            fullscreen = json_object_get_boolean_member_with_default(json_node_get_object(n), "hasfullscreen", FALSE);
        if (n) json_node_unref(n);
    }
    g_free(out);
    apply_state();
    return G_SOURCE_REMOVE;
}

static void read_profile(GDBusProxy *p) {
    GVariant *v = g_dbus_proxy_get_cached_property(p, "ActiveProfile");
    saver = v && g_strcmp0(g_variant_get_string(v, NULL), "power-saver") == 0;
    if (v) g_variant_unref(v);
}
static void on_profile(GDBusProxy *p, GVariant *changed, GStrv inv, gpointer d) { read_profile(p); apply_state(); }

static void watch_power(void) {
    GDBusProxy *p = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
                        "org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles",
                        "org.freedesktop.UPower.PowerProfiles", NULL, NULL);
    if (!p) return;
    read_profile(p);
    g_signal_connect(p, "g-properties-changed", G_CALLBACK(on_profile), NULL);
}

/* ═════════════ ВІКНО ═════════════ */
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer d) {   /* саме вікно GTK — повністю прозоре */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    return TRUE;
}

static void setup_surface(void) {
    GdkDisplay *gd = gdk_display_get_default();
    struct wl_display *dpy = gdk_wayland_display_get_wl_display(gd);
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    struct wl_compositor *comp = gdk_wayland_display_get_wl_compositor(gd);
    if (!wl_subcomp || !wl_shm_g) { g_printerr("stars: немає wl_subcompositor / wl_shm\n"); gtk_main_quit(); return; }
    for (int i = 0; i < NBUF; i++)
        if (!shm_buf(&bufs[i], W * scale, H * scale)) { g_printerr("stars: немає пам'яті під кадр\n"); gtk_main_quit(); return; }

    GdkWindow *gw = gtk_widget_get_window(win);
    cairo_region_t *none = cairo_region_create();             /* кліки — крізь */
    gdk_window_input_shape_combine_region(gw, none, 0, 0);
    cairo_region_destroy(none);

    surf = wl_compositor_create_surface(comp);
    wl_surface_set_buffer_scale(surf, scale);
    struct wl_region *empty = wl_compositor_create_region(comp);
    wl_surface_set_input_region(surf, empty);
    /* і для самого вікна GTK — напряму: GDK застосовує свою форму лише при
       перемальовуванні, а прозоре вікно після першого кадру не перемальовується,
       тож без цього шар на весь екран забирав собі мишку */
    struct wl_surface *parent = gdk_wayland_window_get_wl_surface(gw);
    wl_surface_set_input_region(parent, empty);
    wl_surface_commit(parent);
    wl_region_destroy(empty);
    sub = wl_subcompositor_get_subsurface(wl_subcomp, surf, gdk_wayland_window_get_wl_surface(gw));
    wl_subsurface_set_position(sub, 0, 0);
    wl_subsurface_set_desync(sub);            /* кадри — незалежно від GTK */

    make_sprites();
    on_windows(NULL);
}

static gboolean on_map(GtkWidget *w, GdkEvent *e, gpointer d) {
    if (surf) return FALSE;
    /* розмір — з монітора: на момент показу GTK ще може думати, що вікно 200×200 */
    GdkRectangle g;
    gdk_monitor_get_geometry(gdk_display_get_monitor_at_window(gdk_display_get_default(), gtk_widget_get_window(w)), &g);
    W = g.width; H = g.height;
    scale = gtk_widget_get_scale_factor(w);
    setup_surface();
    return FALSE;
}

static gboolean on_quit(gpointer d) { gtk_main_quit(); return G_SOURCE_REMOVE; }

int main(int argc, char **argv) {
    test_mode = g_getenv("STARS_TEST_OVERLAY") != NULL;
    hw_single(test_mode ? "stars-test" : "stars", (const int[]){ SIGHUP, 0 });
    gtk_init(&argc, &argv);
    shown = g_array_new(FALSE, FALSE, sizeof(Rect));

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GdkVisual *vis = gdk_screen_get_rgba_visual(gtk_widget_get_screen(win));
    if (vis) gtk_widget_set_visual(win, vis);
    gtk_widget_set_app_paintable(win, TRUE);
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_namespace(GTK_WINDOW(win), "stars");
    gtk_layer_set_layer(GTK_WINDOW(win), test_mode ? GTK_LAYER_SHELL_LAYER_OVERLAY
                        : OVER_WINDOWS ? GTK_LAYER_SHELL_LAYER_TOP : GTK_LAYER_SHELL_LAYER_BACKGROUND);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);           /* на весь екран, ігноруючи waybar */
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++) gtk_layer_set_anchor(GTK_WINDOW(win), e, TRUE);
    g_signal_connect(win, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(win, "map-event", G_CALLBACK(on_map), NULL);
    gtk_widget_show_all(win);

    hw_listen(on_windows);                    /* вікна змінились → чи немає повноекранного */
    watch_power();
    g_unix_signal_add(SIGHUP, on_windows, NULL);
    g_unix_signal_add(SIGTERM, on_quit, NULL);
    g_unix_signal_add(SIGINT, on_quit, NULL);
    gtk_main();
    return 0;
}
