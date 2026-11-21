/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  PIP — відео «картинка в картинці» поверх усього (C + GTK4)  │
 * │                                                              │
 * │  pip <посилання>   YouTube, серіали, аніме — усе, що вміє    │
 * │                    yt-dlp (як mpv), або файл. Уже працює —   │
 * │                    нове відео замінює поточне.               │
 * │                                                              │
 * │  Вікно в правому нижньому куті, над усіма вікнами; без       │
 * │  рамок. Навів мишку — знизу кнопки (перемотка, пауза,        │
 * │  прозорість, замок, сховати, закрити). Мишею (коли не        │
 * │  замкнене): тягнути — перенести,                             │
 * │  коліщатко — більше / менше, подвійний клік — пауза.         │
 * │  Як віджети столу: тягнеться по сітці 20 px, кадри — 24/с на │
 * │  спільній сітці кадрів; показати / сховати — «%» у меню SUPER │
 * │  або desk.sh toggle video. Позиція й розмір запам'ятовуються │
 * │  (~/.cache/pip.pos).                                         │
 * │                                                              │
 * │  Керування — ~/.local/bin/pipctl (клавіші SUPER+Alt+…):      │
 * │    показати / сховати · замок (кліки проходять крізь відео)  │
 * │    прозорість · пауза · перемотка · закрити                  │
 * │  Команди приходять через mpv (script-message pip …).         │
 * │                                                              │
 * │  Відео декодує відеочип (hwdec), малює — теж (libmpv →       │
 * │  OpenGL): процесор майже не навантажений.                    │
 * │  Збірка — ~/.config/hypr/src/build.sh pip                    │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gdk/wayland/gdkwayland.h>
#include <glib-unix.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hyprwatch.h"
#include "theme.h"

/* ───────────── НАЛАШТУВАННЯ ───────────── */
#define DEF_W     480        /* розмір за замовчуванням (16:9) */
#define DEF_H     270
#define MIN_W     240
#define MAX_W     1280
#define DEF_GAP   20         /* відступ від правого й нижнього країв */
#define GRID      20         /* сітка перетягування — та сама, що в віджетів столу (desk) */
#define SIZE_STEP 40         /* крок розміру коліщатком (2 клітинки сітки) */
#define FPS       24         /* кадрів/с — на спільній сітці кадрів з еквалайзером і хвилею (hyprwatch) */
/* Формат: YouTube віддає 403 на прямі посилання — беремо потоки HLS (m3u8): відео до 480p окремо
 * (чітко навіть у збільшеному вікні, а потік — удвічі менший, ніж 720p) +
 * звук окремо, з ОРИГІНАЛЬНОЮ доріжкою (а не автоперекладом); інакше — що є */
/* Іконки кнопок (Nerd Font, кодами) */
#define I_BACK  "\U000F0D2A"     /* rewind_10: −10 с */
#define I_PLAY  "\U000F040A"
#define I_PAUSE "\U000F03E4"
#define I_FWD   "\U000F0D71"     /* fast_forward_10: +10 с */
#define I_OPAC  "\U000F05CC"     /* opacity: видимість 100% ↔ 5% */
#define I_LOCK  "\U000F033E"
#define I_HIDE  "\U000F0209"
#define I_CLOSE "\U000F0156"
#define FORMAT    "bv*[protocol^=m3u8][height<=?480]+ba[protocol^=m3u8][format_note*=original]/bv*[protocol^=m3u8][height<=?480]+ba[protocol^=m3u8]/b[protocol^=m3u8][height<=?480]/bv*[height<=?480]+ba/b"

static GtkWidget *win, *area, *bar, *btn_play;
static guint bar_timer;
static mpv_handle *mpv;
static mpv_render_context *rctx;
static int w = DEF_W, h = DEF_H, mright = DEF_GAP, mbottom = DEF_GAP;
static double opacity = 1.0;
static gboolean locked = FALSE, dragging = FALSE;
static double press_x, press_y;          /* де натиснули — у координатах вікна */

/* ═════════════ ПОЗИЦІЯ ТА РОЗМІР ═════════════ */
static char *pos_file(void) { return g_build_filename(g_get_user_cache_dir(), "pip.pos", NULL); }
static int snap(int v) { return MAX(0, (int)floor((v + GRID / 2.0) / GRID) * GRID); }

static void load_pos(void) {
    char *f = pos_file(), *s = NULL;
    if (g_file_get_contents(f, &s, NULL, NULL)) {
        char op[32] = "";
        if (sscanf(s, "%d %d %d %31s", &mright, &mbottom, &w, op) >= 3 && *op) opacity = g_ascii_strtod(op, NULL);
    }
    g_free(s); g_free(f);
    w = CLAMP(w / SIZE_STEP * SIZE_STEP, MIN_W, MAX_W); h = w * 9 / 16;
    mright = snap(mright); mbottom = snap(mbottom);
    opacity = CLAMP(opacity, 0.05, 1.0);
}
static void save_pos(void) {
    char op[G_ASCII_DTOSTR_BUF_SIZE];
    char *f = pos_file(), *s = g_strdup_printf("%d %d %d %s\n", mright, mbottom, w, g_ascii_formatd(op, sizeof op, "%.2f", opacity));
    g_file_set_contents(f, s, -1, NULL);
    g_free(s); g_free(f);
}
static void apply_geometry(void) {
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, mright);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, mbottom);
    gtk_widget_set_size_request(area, w, h);
    gtk_window_set_default_size(GTK_WINDOW(win), w, h);
}

/* ═════════════ ЗАМОК: кліки проходять крізь відео ═════════════ */
static void apply_lock(void) {
    GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(win));
    if (!s) return;
    if (locked) {
        cairo_region_t *empty = cairo_region_create();
        gdk_surface_set_input_region(s, empty);
        cairo_region_destroy(empty);
    } else {
        gdk_surface_set_input_region(s, NULL);            /* NULL — усе вікно знову приймає мишку */
    }
}

static void osd(const char *text) {                      /* коротке повідомлення поверх відео */
    const char *cmd[] = { "show-text", text, "1200", NULL };
    mpv_command_async(mpv, 0, cmd);
}

/* ═════════════ КОМАНДИ (pipctl → script-message pip …) ═════════════ */
static void command(const char *what, const char *arg) {
    char buf[64];
    if (!g_strcmp0(what, "show")) {                       /* нове відео — показати, якщо сховане */
        if (!gtk_widget_get_visible(win)) { gtk_widget_set_visible(win, TRUE); apply_lock(); }
    } else if (!g_strcmp0(what, "toggle")) {
        gboolean vis = !gtk_widget_get_visible(win);
        gtk_widget_set_visible(win, vis);
        if (vis) apply_lock();
    } else if (!g_strcmp0(what, "lock")) {
        locked = !locked;
        apply_lock();
        osd(locked ? "\U000F033E Замкнено: кліки проходять крізь відео" : "\U000F0FC6 Відімкнено");
    } else if (!g_strcmp0(what, "opacity") && arg) {
        opacity = CLAMP(opacity + g_ascii_strtod(arg, NULL), 0.05, 1.0);
        gtk_widget_set_opacity(area, opacity);
        g_snprintf(buf, sizeof buf, "Видимість: %d%%", (int)round(opacity * 100));
        osd(buf);
        save_pos();
    }
}

/* ═════════════ MPV ═════════════ */
static void *get_proc(void *ctx, const char *name) { return (void *)eglGetProcAddress(name); }

static gboolean on_mpv_events(gpointer d) {
    while (mpv) {
        mpv_event *e = mpv_wait_event(mpv, 0);
        if (e->event_id == MPV_EVENT_NONE) break;
        if (e->event_id == MPV_EVENT_SHUTDOWN) { gtk_window_destroy(GTK_WINDOW(win)); return G_SOURCE_REMOVE; }
        if (e->event_id == MPV_EVENT_PROPERTY_CHANGE) {          /* пауза ↔ грає — іконка кнопки */
            mpv_event_property *p = e->data;
            if (!strcmp(p->name, "pause") && p->format == MPV_FORMAT_FLAG && btn_play)
                gtk_button_set_label(GTK_BUTTON(btn_play), *(int *)p->data ? I_PLAY : I_PAUSE);
        }
        if (e->event_id == MPV_EVENT_CLIENT_MESSAGE) {
            mpv_event_client_message *m = e->data;
            if (m->num_args >= 2 && !g_strcmp0(m->args[0], "pip")) command(m->args[1], m->num_args > 2 ? m->args[2] : NULL);
        }
    }
    return G_SOURCE_REMOVE;
}
static void wakeup(void *d) { g_idle_add(on_mpv_events, NULL); }       /* з потоку mpv → у головний */

/* Новий кадр від mpv не малюємо одразу, а в найближчій точці спільної сітки кадрів (24/с):
 * разом з еквалайзером і хвилею — Hyprland складає екран один раз; зайві кадри mpv пропускає сам. */
static HwTick draw_tick;
static gboolean frame_ready;
static int idle_ticks;
static gboolean draw_step(gpointer d) {
    if (frame_ready) { frame_ready = FALSE; idle_ticks = 0; gtk_gl_area_queue_render(GTK_GL_AREA(area)); return TRUE; }
    return ++idle_ticks < FPS;                           /* секунду без нових кадрів (пауза) — таймер стоп */
}
static gboolean frame_arrived(gpointer d) { frame_ready = TRUE; hw_tick_start(&draw_tick, FPS, draw_step); return G_SOURCE_REMOVE; }
static void on_update(void *d) { g_idle_add(frame_arrived, NULL); }   /* з потоку mpv → у головний */

static void on_realize(GtkGLArea *a, gpointer d) {
    gtk_gl_area_make_current(a);
    if (gtk_gl_area_get_error(a)) return;
    mpv_opengl_init_params gl = { get_proc, NULL };
    mpv_render_param p[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl },
        /* з'єднання з Wayland — без нього mpv не відкриє відеочип для декодування (VA-API) */
        { MPV_RENDER_PARAM_WL_DISPLAY, gdk_wayland_display_get_wl_display(gdk_display_get_default()) },
        { 0 }
    };
    if (mpv_render_context_create(&rctx, mpv, p) < 0) { g_printerr("pip: не вдалося запустити відео\n"); return; }
    mpv_render_context_set_update_callback(rctx, on_update, NULL);
}

static gboolean on_render(GtkGLArea *a, GdkGLContext *c, gpointer d) {
    if (!rctx) return TRUE;
    int fbo = 0, scale = gtk_widget_get_scale_factor(GTK_WIDGET(a));
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    mpv_opengl_fbo f = { fbo, gtk_widget_get_width(GTK_WIDGET(a)) * scale, gtk_widget_get_height(GTK_WIDGET(a)) * scale, 0 };
    mpv_render_param p[] = { { MPV_RENDER_PARAM_OPENGL_FBO, &f }, { MPV_RENDER_PARAM_FLIP_Y, &(int){ 1 } }, { 0 } };
    mpv_render_context_render(rctx, p);
    if (g_getenv("PIP_DEBUG")) {
        static int n;
        if (n++ % 60 == 0) g_printerr("pip render %d: %.2f с, fbo=%d %dx%d масштаб=%d\n", n, g_get_monotonic_time() / 1e6, fbo, f.w, f.h, scale);
    }
    return TRUE;
}

static void on_unrealize(GtkGLArea *a, gpointer d) {
    gtk_gl_area_make_current(a);
    if (rctx) { mpv_render_context_free(rctx); rctx = NULL; }
}

/* ═════════════ КНОПКИ ПРИ НАВЕДЕННІ ═════════════ */

static void mpv_cmd(const char *a, const char *b, const char *c) {
    const char *cmd[] = { a, b, c, NULL };
    mpv_command_async(mpv, 0, cmd);
}
static gboolean bar_hide(gpointer d) { bar_timer = 0; gtk_widget_set_visible(bar, FALSE); return G_SOURCE_REMOVE; }
static void bar_show(void) {                          /* показати й сховати через 2 с без руху мишки */
    gtk_widget_set_visible(bar, TRUE);
    if (bar_timer) g_source_remove(bar_timer);
    bar_timer = g_timeout_add(2000, bar_hide, NULL);
}
static void on_button(GtkButton *b, gpointer what) {
    const char *w = what;
    if (!strcmp(w, "back"))       mpv_cmd("seek", "-10", NULL);
    else if (!strcmp(w, "fwd"))   mpv_cmd("seek", "10", NULL);
    else if (!strcmp(w, "play"))  mpv_cmd("cycle", "pause", NULL);
    else if (!strcmp(w, "close")) mpv_cmd("quit", NULL, NULL);
    else if (!strcmp(w, "opac")) {                      /* видимість: 100% ↔ 5% */
        opacity = opacity > 0.5 ? 0.05 : 1.0;           /* 100% ↔ 5% (ледь видно) */
        gtk_widget_set_opacity(area, opacity);
        save_pos();
    } else command(w, NULL);                            /* lock, toggle (сховати) */
}
static GtkWidget *button(const char *icon, const char *what, const char *tip) {
    GtkWidget *b = gtk_button_new_with_label(icon);
    gtk_widget_add_css_class(b, "pip-btn");
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focusable(b, FALSE);
    g_signal_connect(b, "clicked", G_CALLBACK(on_button), (gpointer)what);
    gtk_box_append(GTK_BOX(bar), b);
    return b;
}

/* ═════════════ МИША (коли не замкнено) ═════════════ */
/* Перетягування: вікно прив'язане до правого й нижнього країв. Координати мишки — відносно вікна
 * в його ПОТОЧНІЙ позиції: вікно зсунулось на стільки, на скільки мишка відійшла від точки натискання,
 * і мишка знову «на тому ж місці» вікна — без накопичення похибки й смикання. */
static void on_press(GtkGestureClick *g, int n, double x, double y, gpointer d) {
    if (n == 2) { const char *cmd[] = { "cycle", "pause", NULL }; mpv_command_async(mpv, 0, cmd); return; }   /* подвійний — пауза */
    dragging = TRUE; press_x = x; press_y = y;
}
static void on_release(GtkGestureClick *g, int n, double x, double y, gpointer d) {
    if (dragging) { dragging = FALSE; save_pos(); }
}
static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer d) {
    bar_show();
    if (!dragging) return;
    int r = snap(mright - (int)round(x - press_x)), b = snap(mbottom - (int)round(y - press_y));   /* по сітці */
    if (r == mright && b == mbottom) return;
    mright = r; mbottom = b;
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, mright);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, mbottom);
}

static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer d) {   /* коліщатко — розмір */
    w = CLAMP(w - (dy > 0 ? SIZE_STEP : dy < 0 ? -SIZE_STEP : 0), MIN_W, MAX_W);
    h = w * 9 / 16;
    apply_geometry();
    save_pos();
    return TRUE;
}
/* ═════════════ ЗАПУСК ═════════════ */
static const char *opts = "";                              /* параметри mpv для першого відео (argv[2]) */
static void activate(GtkApplication *app, gpointer url) {
    setlocale(LC_NUMERIC, "C");      /* GTK щойно ввімкнув мову системи (кома в числах) — mpv потрібна крапка */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window { background: none; }"
        ".pip-frame { border-radius: " G_STRINGIFY(TH_RADIUS_CARD) "px; }"   /* заокруглені кути (тема) */
        ".pip-bar { background-color: alpha(" TH_BG ", 0.78); border-radius: 12px; padding: 2px 6px; margin-bottom: 10px; }"
        ".pip-btn { background: none; border: none; box-shadow: none; color: " TH_TEXT "; font-family: \"" TH_FONT_ICONS "\";"
        "           font-size: 16px; min-width: 30px; min-height: 30px; padding: 0; border-radius: 8px; }"
        ".pip-btn:hover { background-color: alpha(" TH_ACCENT ", 0.25); color: " TH_ACCENT "; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    win = gtk_application_window_new(app);
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_namespace(GTK_WINDOW(win), "pip");
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);      /* над усім, навіть над повноекранним */
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);   /* клавіатуру не забирає */
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);

    area = gtk_gl_area_new();
    gtk_gl_area_set_auto_render(GTK_GL_AREA(area), FALSE);                    /* малюємо лише нові кадри */
    g_signal_connect(area, "realize", G_CALLBACK(on_realize), NULL);
    g_signal_connect(area, "unrealize", G_CALLBACK(on_unrealize), NULL);
    g_signal_connect(area, "render", G_CALLBACK(on_render), NULL);
    gtk_widget_set_opacity(area, opacity);
    GtkWidget *over = gtk_overlay_new();                                     /* відео + кнопки поверх */
    gtk_overlay_set_child(GTK_OVERLAY(over), area);
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(bar, "pip-bar");
    gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(bar, GTK_ALIGN_END);
    button(I_BACK, "back", "Назад на 10 с (SUPER+Alt+,)");
    btn_play = button(I_PAUSE, "play", "Пауза / грати (SUPER+Alt+Пробіл, подвійний клік)");
    button(I_FWD, "fwd", "Вперед на 10 с (SUPER+Alt+.)");
    button(I_OPAC, "opac", "Видимість: 100% ↔ 5% (точніше — SUPER+Alt+[ / ], крок 5%)");
    button(I_LOCK, "lock", "Замок: кліки проходять крізь відео (зняти — SUPER+Alt+L)");
    button(I_HIDE, "toggle", "Сховати (показати — SUPER+Alt+V)");
    button(I_CLOSE, "close", "Закрити (SUPER+Alt+Q)");
    gtk_widget_set_visible(bar, g_getenv("PIP_DEBUG") != NULL);           /* налагодження — панель видно одразу */
    gtk_overlay_add_overlay(GTK_OVERLAY(over), bar);
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);             /* обгортка: обрізає по заокругленню */
    gtk_widget_add_css_class(frame, "pip-frame");
    gtk_widget_set_overflow(frame, GTK_OVERFLOW_HIDDEN);
    gtk_box_append(GTK_BOX(frame), over);
    gtk_window_set_child(GTK_WINDOW(win), frame);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_press), NULL);
    g_signal_connect(click, "released", G_CALLBACK(on_release), NULL);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), NULL);
    gtk_widget_add_controller(area, motion);
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), NULL);
    gtk_widget_add_controller(area, scroll);

    apply_geometry();
    gtk_window_present(GTK_WINDOW(win));

    const char *cmd[] = { "loadfile", url, "replace", "-1", opts, NULL };   /* opts — referrer/user-agent (pipctl) */
    mpv_command_async(mpv, 0, cmd);
}

static gboolean on_quit(gpointer app) { g_application_quit(G_APPLICATION(app)); return G_SOURCE_REMOVE; }

int main(int argc, char **argv) {
    if (argc < 2) { g_printerr("pip <посилання на відео> [параметри mpv: referrer=…,user-agent=…]\n"); return 2; }
    opts = argc > 2 ? argv[2] : "";
    hw_single("pip", NULL);                                /* уже працює — нове відео відкриває pipctl */
    setlocale(LC_NUMERIC, "C");                            /* libmpv вимагає крапку в числах */
    load_pos();

    mpv = mpv_create();
    char *sock = g_build_filename(g_get_user_runtime_dir(), "pip-mpv.sock", NULL);
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "load-scripts", "no");      /* без /etc/mpv/scripts (mpris.so): інакше відео видно як
                                                              ще один «mpv» — music-active ставить музику на паузу */
    mpv_set_option_string(mpv, "hwdec", "auto-safe");      /* декодує відеочип */
    mpv_set_option_string(mpv, "input-ipc-server", sock);  /* сюди пише pipctl */
    mpv_set_option_string(mpv, "ytdl-format", FORMAT);
    mpv_set_option_string(mpv, "keep-open", "no");         /* відео скінчилось — вікно закривається */
    mpv_set_option_string(mpv, "osd-font", TH_FONT_UI);
    mpv_set_option_string(mpv, "osd-font-size", "28");
    mpv_set_option_string(mpv, "volume", "100");
    if (g_getenv("PIP_DEBUG")) { mpv_set_option_string(mpv, "terminal", "yes"); mpv_set_option_string(mpv, "msg-level", "all=v"); }
    g_free(sock);
    if (mpv_initialize(mpv) < 0) { g_printerr("pip: не вдалося запустити mpv\n"); return 1; }
    mpv_set_wakeup_callback(mpv, wakeup, NULL);
    mpv_observe_property(mpv, 0, "pause", MPV_FORMAT_FLAG);   /* іконка кнопки «пауза» */
    on_mpv_events(NULL);                                   /* події, що прийшли до старту */

    GtkApplication *app = gtk_application_new(NULL, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), argv[1]);
    g_unix_signal_add(SIGTERM, on_quit, app);
    g_unix_signal_add(SIGINT, on_quit, app);
    char *gargv[] = { argv[0], NULL };
    int rc = g_application_run(G_APPLICATION(app), 1, gargv);
    if (rctx) mpv_render_context_free(rctx);
    mpv_terminate_destroy(mpv);
    mpv = NULL;
    return rc;
}
