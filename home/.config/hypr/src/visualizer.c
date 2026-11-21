/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  КРУГОВИЙ ЕКВАЛАЙЗЕР — по центру робочого столу (C + GTK4)   │
 * │                                                              │
 * │  cava (~/.config/cava/circle.conf) рахує спектр і віддає     │
 * │  числа (двійково, 16 біт на стовпець). Тут вони малюються    │
 * │  стовпцями, що розходяться від кола назовні.                 │
 * │                                                              │
 * │  Економія:                                                   │
 * │   • малює ВІДЕОЧИП (GTK4 → OpenGL/Vulkan): стовпці — прості  │
 * │     фігури (повернуті прямокутники), процесор їх не рахує;   │
 * │     готовий кадр іде в Hyprland без копіювання;              │
 * │   • кожен стовпець — окремий вузол: оновлюється лише те, що  │
 * │     змінилось, порожній центр не перемальовується;           │
 * │   • коло закрите НЕпрозорим вікном (Firefox…) → cava         │
 * │     зупинено, 0 навантаження. Крізь прозорі вікна (kitty:    │
 * │     термінал, Claude, музика) коло видно — малюємо далі.     │
 * │     Стежимо за подіями Hyprland (socket2), без опитування;   │
 * │   • у тиші cava засинає; однакові кадри пропускаються;       │
 * │   • кадри — на спільній «сітці» з хвилею музики (hyprwatch): │
 * │     обидві анімації малюють в один момент → Hyprland         │
 * │     складає екран раз на обидві.                             │
 * │                                                              │
 * │  По центру — годинник «ГГ:ХХ».                               │
 * │  Усередині кола — секундний циферблат: 60 поділок, поточна   │
 * │  секунда яскрава, пройдені за хвилину — підсвічені.          │
 * │  У тиші перемальовується лише раз на секунду.                │
 * │                                                              │
 * │  Вікно — лише розміром з коло, шар «фон» (під усіма          │
 * │  вікнами), кліки проходять крізь нього.                      │
 * │  Кольори й шрифт — з теми (~/.config/theme/theme.conf).      │
 * │  Вмикати / вимикати — ~/.config/hypr/desk.sh toggle visualizer│
 * │  Збірка — ~/.config/hypr/src/build.sh                        │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "hyprwatch.h"
#include "theme.h"

/* ───────────── НАЛАШТУВАННЯ ───────────── */
#define RADIUS   150.0      /* радіус внутрішнього кола (px) */
#define MAX_LEN  170.0      /* найдовший стовпець (px) */
#define MIN_LEN  3.0        /* у тиші — короткі «крапки» по колу */
#define FILL     0.55       /* товщина стовпця: частка від проміжку між ними */
#define LEVELS   12         /* відтінків градієнта */
#define MAX_BARS 512
#define VIS_FPS  24         /* = framerate у circle.conf; та сама сітка кадрів, що й у хвилі музики */
/* Кольори від коротких до довгих стовпців (тема) */
static const char *COLORS[] = { TH_GREEN, TH_ACCENT, TH_BLUE, TH_ACCENT2, TH_RED };
#define N_COLORS (int)G_N_ELEMENTS(COLORS)
static const GdkRGBA RING = { TH_MUTED_RGB, 0.35f };          /* тонке коло всередині */
/* Секундний циферблат усередині кола: 60 поділок, поточна секунда яскрава */
#define SEC_RADIUS (RADIUS - 22)    /* радіус поділок */
static const GdkRGBA SEC_NOW  = { TH_ACCENT2_RGB, 1.00f };    /* поточна секунда */
static const GdkRGBA SEC_PAST = { TH_ACCENT_RGB,  0.95f };    /* пройдені секунди хвилини */
static const GdkRGBA SEC_LEFT = { TH_MUTED_RGB,   0.32f };    /* ще не пройдені */
/* Годинник по центру: години й хвилини; моноширинний — цифри однакової ширини */
#define CLOCK_FONT TH_FONT_MONO " Light 27"
static const GdkRGBA CLOCK_COLOR = { TH_TEXT_RGB, 0.90f };

#define SIZE ((int)(2 * (RADIUS + MAX_LEN) + 20))

/* ───────────── СТАН ───────────── */
static GtkWidget *area;
static GdkRGBA level_rgb[LEVELS];
static float bars[MAX_BARS];
static int n_bars = 0;
static double angle_deg[MAX_BARS];
static int angles_n = 0;   /* для скількох стовпців пораховано angle_deg */
static HwTick draw_tick;   /* малювання на спільній сітці кадрів (hyprwatch) */
static gboolean dirty;     /* від cava прийшов новий кадр, ще не намальований */
static int idle_ticks;     /* скільки точок сітки поспіль без нових даних */

static GPid cava_pid = 0;
static int cava_fd = -1;
static guint cava_watch = 0;
static guint8 *cbuf = NULL;
static gsize cbuf_len = 0;
static guint sec_timer = 0;                /* перемальовування щосекунди (годинник) */
static PangoLayout *clock_layout = NULL;   /* «ГГ:ХХ» — перебудовується лише коли змінився текст */
static char clock_text[8] = "";

/* ═════════════ КОЛЬОРИ ═════════════ */
static void build_levels(void) {
    GdkRGBA pal[N_COLORS];
    for (int i = 0; i < N_COLORS; i++) gdk_rgba_parse(&pal[i], COLORS[i]);
    for (int k = 0; k < LEVELS; k++) {
        double t = (double)k / (LEVELS - 1) * (N_COLORS - 1);
        int i = MIN((int)t, N_COLORS - 2);
        double f = t - i;
        level_rgb[k] = (GdkRGBA){ pal[i].red + (pal[i + 1].red - pal[i].red) * f,
                                  pal[i].green + (pal[i + 1].green - pal[i].green) * f,
                                  pal[i].blue + (pal[i + 1].blue - pal[i].blue) * f, 1.0f };
    }
}

static void build_angles(int n) {          /* кути стовпців — рахуються один раз */
    if (n == angles_n) return;
    for (int i = 0; i < n; i++)             /* від верхньої точки за годинниковою стрілкою */
        angle_deg[i] = -90.0 + 360.0 * i / n;
    angles_n = n;
}

/* ═════════════ МАЛЮВАННЯ (віджет) ═════════════ */
#define VIS_TYPE_RING (vis_ring_get_type())
G_DECLARE_FINAL_TYPE(VisRing, vis_ring, VIS, RING, GtkWidget)
struct _VisRing { GtkWidget parent; };
G_DEFINE_TYPE(VisRing, vis_ring, GTK_TYPE_WIDGET)

/* Кожен стовпець — повернутий прямокутник із заокругленими кінцями.
 * Такі вузли (колір + заокруглене обрізання + поворот) GTK віддає відеочипу напряму,
 * без розрахунку на процесорі (на відміну від «шляхів»/stroke). Кожен стовпець — свій
 * вузол зі своїми межами, тож перемальовується лише те, що змінилось. */
static void vis_ring_snapshot(GtkWidget *w, GtkSnapshot *s) {
    const float cx = SIZE / 2.0f, cy = SIZE / 2.0f;

    /* тонке коло — рамка круглого прямокутника */
    const float rr = RADIUS - 6;
    GskRoundedRect circle;
    gsk_rounded_rect_init_from_rect(&circle, &GRAPHENE_RECT_INIT(cx - rr, cy - rr, 2 * rr, 2 * rr), rr);
    const float bw[4] = { 1.5f, 1.5f, 1.5f, 1.5f };
    const GdkRGBA bc[4] = { RING, RING, RING, RING };
    gtk_snapshot_append_border(s, &circle, bw, bc);

    /* Секунди: 60 поділок по колу; кожна 5-та довша. Перемальовується щонайменше раз на секунду. */
    GDateTime *now = g_date_time_new_now_local();
    int sec = g_date_time_get_second(now);

    /* Годинник по центру: «ГГ:ХХ» */
    char hm[8];
    g_snprintf(hm, sizeof hm, "%02d:%02d", g_date_time_get_hour(now), g_date_time_get_minute(now));
    g_date_time_unref(now);
    if (!clock_layout) {
        clock_layout = gtk_widget_create_pango_layout(w, NULL);
        PangoFontDescription *fd = pango_font_description_from_string(CLOCK_FONT);
        pango_layout_set_font_description(clock_layout, fd);
        pango_font_description_free(fd);
    }
    if (strcmp(hm, clock_text) != 0) {                 /* нова хвилина */
        g_strlcpy(clock_text, hm, sizeof clock_text);
        pango_layout_set_text(clock_layout, clock_text, -1);
    }
    /* Центруємо за ЛОГІЧНИМИ межами (повна ширина знаків, однакова для будь-яких цифр),
     * а не за обрисами — інакше «1» вужча за «0», і текст зсувався б щохвилини. */
    PangoRectangle logical;
    pango_layout_get_pixel_extents(clock_layout, NULL, &logical);
    gtk_snapshot_save(s);
    gtk_snapshot_translate(s, &GRAPHENE_POINT_INIT(cx - logical.x - logical.width / 2.0f,
                                                   cy - logical.y - logical.height / 2.0f));
    gtk_snapshot_append_layout(s, clock_layout, &CLOCK_COLOR);
    gtk_snapshot_restore(s);
    for (int j = 0; j < 60; j++) {
        gboolean cur = j == sec, five = j % 5 == 0;
        float len = cur ? 20.0f : five ? 11.0f : 6.0f;   /* довжина мітки */
        float th  = cur ? 6.0f  : five ? 3.5f  : 2.5f;   /* товщина мітки */
        const GdkRGBA *col = cur ? &SEC_NOW : j < sec ? &SEC_PAST : &SEC_LEFT;
        graphene_rect_t r = GRAPHENE_RECT_INIT(SEC_RADIUS - len, -th / 2, len, th);   /* мітка всередину від кола */
        GskRoundedRect rounded;
        gsk_rounded_rect_init_from_rect(&rounded, &r, th / 2);
        gtk_snapshot_save(s);
        gtk_snapshot_translate(s, &GRAPHENE_POINT_INIT(cx, cy));
        gtk_snapshot_rotate(s, -90.0f + 6.0f * j);      /* 0-ва секунда — вгорі, далі за годинниковою */
        gtk_snapshot_push_rounded_clip(s, &rounded);
        gtk_snapshot_append_color(s, col, &r);
        gtk_snapshot_pop(s);
        gtk_snapshot_restore(s);
    }

    int n = n_bars ? n_bars : 120;
    build_angles(n);
    const float thick = MAX(1.0f, (float)(2 * G_PI * RADIUS / n * FILL));
    for (int i = 0; i < n; i++) {
        float v = n_bars ? bars[i] : 0.0f;
        int k = MIN(LEVELS - 1, (int)(v * LEVELS));
        float len = MIN_LEN + v * MAX_LEN;
        graphene_rect_t r = GRAPHENE_RECT_INIT(RADIUS, -thick / 2, len, thick);
        GskRoundedRect rounded;
        gsk_rounded_rect_init_from_rect(&rounded, &r, thick / 2);   /* круглі кінці */
        gtk_snapshot_save(s);
        gtk_snapshot_translate(s, &GRAPHENE_POINT_INIT(cx, cy));
        gtk_snapshot_rotate(s, (float)angle_deg[i]);                 /* від верхньої точки за годинниковою */
        gtk_snapshot_push_rounded_clip(s, &rounded);
        gtk_snapshot_append_color(s, &level_rgb[k], &r);
        gtk_snapshot_pop(s);
        gtk_snapshot_restore(s);
    }
}

static void vis_ring_measure(GtkWidget *w, GtkOrientation o, int for_size,
                             int *min, int *nat, int *min_base, int *nat_base) {
    *min = *nat = SIZE;
}

static void vis_ring_class_init(VisRingClass *k) {
    GTK_WIDGET_CLASS(k)->snapshot = vis_ring_snapshot;
    GTK_WIDGET_CLASS(k)->measure = vis_ring_measure;
}
static void vis_ring_init(VisRing *self) {}

/* ═════════════ CAVA ═════════════ */
static gboolean on_cava(gint fd, GIOCondition cond, gpointer d);
static gboolean recheck(gpointer d);

/* Точка сітки кадрів: є нові дані — малюємо; секунду без даних (тиша) — таймер зупиняється */
static gboolean draw_step(gpointer d) {
    if (dirty) { dirty = FALSE; idle_ticks = 0; gtk_widget_queue_draw(area); return TRUE; }
    return ++idle_ticks < VIS_FPS;
}

static void cava_stop(void) {
    if (!cava_pid) return;
    if (cava_watch) { g_source_remove(cava_watch); cava_watch = 0; }
    kill(cava_pid, SIGTERM);                             /* прибере її on_cava_exit (без «зомбі») */
    close(cava_fd);
    cava_pid = 0; cava_fd = -1; cbuf_len = 0;
    n_bars = 0;
    hw_tick_stop(&draw_tick);
}

static void on_cava_exit(GPid pid, gint status, gpointer d) {
    g_spawn_close_pid(pid);                              /* GLib уже забрав статус — процес прибрано */
}

static void cava_start(void) {
    if (cava_pid) return;
    char *conf = g_build_filename(g_get_home_dir(), ".config/cava/circle.conf", NULL);
    char *argv[] = { "cava", "-p", conf, NULL };
    GError *err = NULL;
    if (g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL, NULL, &cava_pid, NULL, &cava_fd, NULL, &err)) {
        g_child_watch_add(cava_pid, on_cava_exit, NULL);
        g_unix_set_fd_nonblocking(cava_fd, TRUE, NULL);
        cava_watch = g_unix_fd_add(cava_fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_cava, NULL);
    } else {
        g_printerr("visualizer: не вдалося запустити cava: %s\n", err->message);
        g_clear_error(&err);
    }
    g_free(conf);
}

/* Дані: кадр = bars × uint16 (0…65535). Беремо лише найсвіжіший повний кадр. */
static gboolean on_cava(gint fd, GIOCondition cond, gpointer d) {
    if (cond & (G_IO_HUP | G_IO_ERR)) {                  /* cava завершилась — спробувати пізніше */
        cava_watch = 0;
        cava_stop();
        g_timeout_add_seconds(5, recheck, NULL);
        return G_SOURCE_REMOVE;
    }
    guint8 tmp[16384];
    ssize_t r;
    while ((r = read(fd, tmp, sizeof tmp)) > 0) {
        cbuf = g_realloc(cbuf, cbuf_len + r);
        memcpy(cbuf + cbuf_len, tmp, r);
        cbuf_len += r;
    }
    static int frame_bars = 0;
    if (!frame_bars) {                                   /* кількість стовпців — з circle.conf */
        char *conf = g_build_filename(g_get_home_dir(), ".config/cava/circle.conf", NULL);
        GKeyFile *kf = g_key_file_new();
        g_key_file_set_list_separator(kf, ';');
        if (g_key_file_load_from_file(kf, conf, G_KEY_FILE_NONE, NULL)) {
            char *v = g_key_file_get_string(kf, "general", "bars", NULL);
            if (v) frame_bars = atoi(v);
            g_free(v);
        }
        g_key_file_free(kf); g_free(conf);
        if (frame_bars <= 0 || frame_bars > MAX_BARS) frame_bars = 120;
    }
    gsize fsz = (gsize)frame_bars * 2;
    if (cbuf_len < fsz) return G_SOURCE_CONTINUE;
    gsize last = (cbuf_len / fsz - 1) * fsz;
    const guint16 *f = (const guint16 *)(cbuf + last);

    float maxd = n_bars == frame_bars ? 0 : 1;
    float nb[MAX_BARS];
    for (int i = 0; i < frame_bars; i++) {
        nb[i] = f[i] / 65535.0f;
        if (n_bars == frame_bars) maxd = MAX(maxd, fabsf(nb[i] - bars[i]));
    }
    memmove(cbuf, cbuf + last + fsz, cbuf_len - last - fsz);
    cbuf_len -= last + fsz;
    if (maxd < 0.004f) return G_SOURCE_CONTINUE;          /* кадр майже той самий — не малювати */
    memcpy(bars, nb, sizeof(float) * frame_bars);
    n_bars = frame_bars;
    dirty = TRUE;                                        /* намалюється в найближчій точці сітки кадрів */
    hw_tick_start(&draw_tick, VIS_FPS, draw_step);
    return G_SOURCE_CONTINUE;
}

/* Годинник: перемалювати рівно на межі кожної секунди (поки коло видно) */
static gboolean on_second(gpointer d) {
    gtk_widget_queue_draw(area);
    gint64 us = g_get_real_time() % G_USEC_PER_SEC;
    sec_timer = g_timeout_add((guint)((G_USEC_PER_SEC - us) / 1000) + 5, on_second, NULL);
    return G_SOURCE_REMOVE;
}
static void clock_start(void) { if (!sec_timer) on_second(NULL); }
static void clock_stop(void)  { if (sec_timer) { g_source_remove(sec_timer); sec_timer = 0; } }

/* ═════════════ ВИДИМІСТЬ: коло закрите непрозорим вікном → нічого не робимо ═════════════ */
static HwRect center_rect(HwRect m, gpointer d) {        /* коло по центру монітора */
    double r = SIZE / 2.0 - 10;
    return (HwRect){ m.x + m.w / 2 - r, m.y + m.h / 2 - r, 2 * r, 2 * r };
}

static gboolean recheck(gpointer d) {
    if (hw_visible(center_rect, NULL)) { cava_start(); clock_start(); }
    else { cava_stop(); clock_stop(); }                  /* закрите — ні спектра, ні годинника */
    return G_SOURCE_REMOVE;
}

/* ═════════════ ВІКНО ═════════════ */
static gboolean on_quit(gpointer app) {
    cava_stop();
    g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

static void on_realize(GtkWidget *win, gpointer d) {
    cairo_region_t *empty = cairo_region_create();       /* кліки проходять крізь вікно */
    gdk_surface_set_input_region(gtk_native_get_surface(GTK_NATIVE(win)), empty);
    cairo_region_destroy(empty);
}

static void activate(GtkApplication *app, gpointer d) {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window { background: none; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *win = gtk_application_window_new(app);
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_namespace(GTK_WINDOW(win), "desktop-visualizer");
    gtk_layer_set_layer(GTK_WINDOW(win), g_getenv("DESK_TEST_OVERLAY")   /* для перевірки вигляду: поверх усього */
                        ? GTK_LAYER_SHELL_LAYER_OVERLAY : GTK_LAYER_SHELL_LAYER_BACKGROUND);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);   /* не зважати на waybar — рівно по центру */
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    /* без прив'язки до країв → вікно по центру екрана */
    area = g_object_new(VIS_TYPE_RING, NULL);
    gtk_window_set_child(GTK_WINDOW(win), area);
    g_signal_connect(win, "realize", G_CALLBACK(on_realize), NULL);
    gtk_window_present(GTK_WINDOW(win));

    hw_listen(recheck);
    recheck(NULL);
}

int main(int argc, char **argv) {
    hw_single(g_getenv("DESK_TEST_OVERLAY") ? "visualizer-test" : "visualizer", NULL);
    build_levels();
    GtkApplication *app = gtk_application_new(NULL, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_unix_signal_add(SIGTERM, on_quit, app);
    g_unix_signal_add(SIGINT, on_quit, app);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
