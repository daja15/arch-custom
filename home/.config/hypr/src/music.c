/* МУЗИКА — див. desk.c */
#include "desk.h"
#include <gdk/gdkwayland.h>
#include <wayland-client.h>
#include <sys/mman.h>
#include <unistd.h>

#define WAVE_AMP    3.0      /* висота хвилі (px) */
#define WAVE_LEN    20.0     /* довжина однієї хвилі (px) */
#define WAVE_SPEED  1.4      /* секунд на одну хвилю (менше — швидше) */
#define WAVE_FPS    12       /* кадрів/с: 24 коштувало ~11% ядра Hyprland і ~10% відеокарти (замір 2026-11-15); 12 ділить 24 — на спільній сітці кадрів з еквалайзером */
#define BAR_H       16       /* висота смуги разом із хвилею */
#define BAR_LW      3.5      /* товщина ліній смуги */

Card mus = { .pos_name = "deskmusic.pos", .off_name = "deskmusic.off", .mright = CAL_RIGHT, .mtop = CAL_TOP + CAL_H + GRID };

/* ═════════════ МУЗИКА: D-Bus / MPRIS ═════════════ */
#define MPRIS "org.mpris.MediaPlayer2."
static GtkWidget *lbl_title, *lbl_artist, *bar, *lbl_pos, *lbl_len;
static char *player = NULL;                /* активний: «org.mpris.MediaPlayer2.kew» тощо */
static gboolean playing = FALSE, visible = TRUE;
static gint64 length_us = 0, pos_us = 0, pos_at = 0;   /* остання позиція від плеєра і коли її отримали */
static guint pos_timer = 0, refresh_id = 0;

/* ═════════════ ФРАЗА замість назви пісні ═════════════
 * Плеєра немає — одразу; плеєр на паузі / зупинений — через QUOTE_AFTER_S секунд.
 * Фраза випадкова, але НЕ змінюється, поки музика не заграє знову (наступна тиша — нова фраза).
 * Решта картки (виконавець, смуга, час) — як завжди. */
#define QUOTE_AFTER_S 30
static const char *QUOTES[] = {
    "Ти — легенда цього робочого столу.",
    "Код боїться тебе, а не навпаки.",
    "Сьогодні в тебе все вийде. Як завжди.",
    "Цей робочий стіл зібрано руками генія.",
    "Баги ховаються, коли ти відкриваєш термінал.",
    "Твоя наполегливість — справжня суперсила.",
    "Кожен рядок коду робить тебе сильнішим.",
    "Ти не шукаєш легких шляхів — ти їх прокладаєш.",
    "Маленький крок сьогодні — великий стрибок завтра.",
    "Тиша. Фокус. Ти. Більше нічого не треба.",
    "Компілятор пишається тобою.",
    "Ти — причина, чому цей ноутбук досі не здався.",
    "Складне стає простим, коли за нього береться профі.",
    "Ти вже ближче до мети, ніж учора.",
    "Твої ідеї варті того, щоб їх втілити.",
    "Відпочинок — теж частина перемоги.",
    "Ти робиш світ трохи кращим. Серйозно.",
    "Помилки — це просто чернетки шедевра.",
    "З такою головою можна все.",
    "Ще одна задача? Для тебе це розминка.",
    "Ти — головний герой цієї історії.",
    "Навіть зірки на екрані світять для тебе.",
    "Дисципліна сьогодні — свобода завтра.",
    "Ти вмієш більше, ніж думаєш.",
    "Цей день — твій. Бери його.",
    "Hyprland ще не бачив такого майстра.",
    "Не порівнюй себе з іншими — у тебе свій рівень.",
    "Сила не в швидкості, а в тому, що ти не зупиняєшся.",
    "Рішення вже десь поруч. Ти його знайдеш.",
    "Твій мозок — найкращий процесор у цій кімнаті.",
    "Те, що ти будуєш, працює. І це круто.",
    "Гарний день, щоб зробити щось велике.",
    "Ти — доказ того, що впертість перемагає.",
    "Кава холоне, а твій запал — ніколи.",
    "Ти не просто пишеш код — ти твориш.",
    "Віра в себе — перший коміт до успіху.",
    "Будь-яку гору підкорюють по одному кроку.",
    "Ти заслуговуєш на все, до чого прагнеш.",
    "Тобі вистачає 10 ват, щоб звернути гори.",
    "Зосередься на важливому — решта зачекає.",
    "Сьогодні ти — найкраща версія себе.",
    "Великі речі роблять ті, хто не здається.",
    "Твоя робота має значення.",
    "Ще трохи — і буде готово. Ти зможеш.",
    "Тиша — найкращий саундтрек для геніальних ідей.",
    "Музика мовчить — значить, час творити.",
    "Голова ясна, руки швидкі — вперед.",
    "Ти — та людина, на яку можна покластися.",
    "Кожна перемога починається з рішення спробувати.",
    "Світ потребує саме того, що вмієш ти.",
};
static gboolean quote_on = FALSE;
static guint quote_timer = 0;
static int quote_last = -1;

static GVariant *get_prop(const char *name, const char *prop) {
    GVariant *r = g_dbus_connection_call_sync(bus, name, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties",
                                              "Get", g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", prop),
                                              G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 300, NULL, NULL);
    if (!r) return NULL;
    GVariant *v;
    g_variant_get(r, "(v)", &v);
    g_variant_unref(r);
    return v;
}

static gboolean is_playing(const char *name) {
    GVariant *v = get_prop(name, "PlaybackStatus");
    gboolean p = v && g_strcmp0(g_variant_get_string(v, NULL), "Playing") == 0;
    if (v) g_variant_unref(v);
    return p;
}

/* Активний плеєр — та сама логіка, що в music-ctl: грає → останній активний → будь-який наш */
static char *find_active(void) {
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                              "ListNames", NULL, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 300, NULL, NULL);
    if (!r) return NULL;
    GPtrArray *ours = g_ptr_array_new_with_free_func(g_free);
    GVariantIter *it; const char *n;
    g_variant_get(r, "(as)", &it);
    while (g_variant_iter_next(it, "&s", &n))                 /* mpv — «Пошук», kew — «Бібліотека» */
        if (g_str_has_prefix(n, MPRIS "mpv") || g_str_has_prefix(n, MPRIS "kew")) g_ptr_array_add(ours, g_strdup(n));
    g_variant_iter_free(it);
    g_variant_unref(r);

    char *found = NULL;
    for (guint i = 0; i < ours->len && !found; i++)
        if (is_playing(ours->pdata[i])) found = g_strdup(ours->pdata[i]);
    if (!found) {
        char *f = g_build_filename(g_get_user_runtime_dir(), "music-active", NULL), *last = NULL;
        if (g_file_get_contents(f, &last, NULL, NULL)) {
            char *full = g_strconcat(MPRIS, g_strstrip(last), NULL);
            for (guint i = 0; i < ours->len && !found; i++)
                if (strcmp(ours->pdata[i], full) == 0) found = g_strdup(full);
            g_free(full);
        }
        g_free(last); g_free(f);
    }
    if (!found && ours->len) found = g_strdup(ours->pdata[0]);
    g_ptr_array_unref(ours);
    return found;
}

static gint64 to_us(GVariant *v) {                 /* mpris:length буває x / t / d */
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))  return g_variant_get_int64(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64)) return (gint64)g_variant_get_uint64(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE)) return (gint64)g_variant_get_double(v);
    return 0;
}

static void fmt_time(char *buf, size_t n, gint64 us) {
    gint64 s = MAX(0, us / G_USEC_PER_SEC);
    if (s >= 3600) g_snprintf(buf, n, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else           g_snprintf(buf, n, "%d:%02d", (int)(s / 60), (int)(s % 60));
}

/* ═════════════ МУЗИКА: ХВИЛЯ у власній поверхні Wayland ═════════════
 * Підповерхня (wl_subsurface) поверх місця смуги в картці: GTK її не перемальовує,
 * кадри йдуть прямо в Hyprland. Два буфери в спільній пам'яті (shm) — поки
 * Hyprland читає один, малюємо в інший. */
typedef struct { struct wl_buffer *buf; void *data; cairo_surface_t *cs; gboolean busy; } ShmBuf;
static struct wl_compositor *wl_comp;
static struct wl_subcompositor *wl_subcomp;
static struct wl_shm *wl_shm_g;
static struct wl_surface *wave_surf;
static struct wl_subsurface *wave_sub;
static ShmBuf bufs[2];
static int wave_w = 0, wave_h = 0, wave_scale = 1;
static gboolean frame_pending = FALSE;     /* Hyprland ще не показав попередній кадр */
static cairo_surface_t *wave_strip = NULL; /* хвиля, намальована один раз (довша за смугу на 1 хвилю) */
static double strip_amp = -1;              /* для якої висоти намальовано wave_strip */
static double amp = 0;                     /* поточна висота: 0 (пауза) … WAVE_AMP (грає) */
static HwTick anim;                        /* таймер хвилі на спільній сітці кадрів (hyprwatch) */

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
    int fd = memfd_create("desk-wave", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { if (fd >= 0) close(fd); return FALSE; }
    b->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (b->data == MAP_FAILED) { close(fd); return FALSE; }
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm_g, fd, size);
    b->buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->buf, &buf_listener, b);
    b->cs = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32, w, h, stride);
    b->busy = FALSE;
    return TRUE;
}

/* Хвиля один раз у картинку: ширина смуги + одна довжина хвилі (щоб було куди зсувати) */
static void build_strip(void) {
    if (wave_strip && fabs(strip_amp - amp) < 0.01) return;
    if (!wave_strip)
        wave_strip = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (wave_w + (int)WAVE_LEN * 2) * wave_scale, wave_h * wave_scale);
    cairo_t *cr = cairo_create(wave_strip);
    cairo_scale(cr, wave_scale, wave_scale);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgb(cr, TH_ACCENT_RGB);
    cairo_set_line_width(cr, BAR_LW);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    double cy = wave_h / 2.0;
    for (double x = 0; x <= wave_w + WAVE_LEN * 2; x += 1.5) {
        double y = cy + amp * sin(2 * G_PI * x / WAVE_LEN);
        if (x == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
    cairo_destroy(cr);
    strip_amp = amp;
}

static gint64 now_pos(void) {                      /* позиція зараз: остання від плеєра + скільки минуло */
    gint64 p = pos_us + (playing ? g_get_monotonic_time() - pos_at : 0);
    return length_us > 0 ? MIN(p, length_us) : p;
}

static void wave_draw(void) {
    if (!wave_surf || frame_pending) return;       /* екран ще не показав попередній кадр — пропустити */
    ShmBuf *b = !bufs[0].busy ? &bufs[0] : !bufs[1].busy ? &bufs[1] : NULL;
    if (!b) return;
    build_strip();

    double W = wave_w, cy = wave_h / 2.0, pad = BAR_LW / 2 + 1;
    double frac = length_us > 0 ? CLAMP((double)now_pos() / length_us, 0, 1) : 0;
    double head = pad + frac * (W - 2 * pad);
    cairo_t *cr = cairo_create(b->cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, wave_scale, wave_scale);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (head + 6 < W - pad) {                      /* решта — рівна сіра лінія */
        cairo_set_source_rgb(cr, TH_LINE_RGB);
        cairo_set_line_width(cr, BAR_LW);
        cairo_move_to(cr, head + 6, cy);
        cairo_line_to(cr, W - pad, cy);
        cairo_stroke(cr);
    }
    if (head > pad) {                              /* зігране — готова хвиля, зсунута за часом */
        double shift = fmod(g_get_monotonic_time() / 1e6 / WAVE_SPEED, 1.0) * WAVE_LEN;
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, head, wave_h);
        cairo_clip(cr);
        cairo_scale(cr, 1.0 / wave_scale, 1.0 / wave_scale);
        cairo_set_source_surface(cr, wave_strip, (pad - WAVE_LEN + shift) * wave_scale, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_set_source_rgb(cr, TH_ACCENT_RGB);            /* голівка — коротка вертикальна риска */
    cairo_set_line_width(cr, 4);
    cairo_move_to(cr, head, cy - 6);
    cairo_line_to(cr, head, cy + 6);
    cairo_stroke(cr);
    cairo_destroy(cr);
    cairo_surface_flush(b->cs);

    wl_surface_attach(wave_surf, b->buf, 0, 0);
    wl_surface_damage(wave_surf, 0, 0, wave_w, wave_h);   /* damage_buffer немає: GTK3 бере wl_compositor v3 */
    struct wl_callback *cb = wl_surface_frame(wave_surf);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    frame_pending = TRUE;
    b->busy = TRUE;
    wl_surface_commit(wave_surf);
    wl_display_flush(gdk_wayland_display_get_wl_display(gdk_display_get_default()));
}

/* Створити підповерхню, коли картка музики вже показана й має розміри */
static void wave_setup(void) {
    if (wave_surf || !gtk_widget_get_mapped(mus.win)) return;
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_WAYLAND_DISPLAY(gd)) return;
    if (!wl_subcomp) {
        struct wl_display *dpy = gdk_wayland_display_get_wl_display(gd);
        struct wl_registry *reg = wl_display_get_registry(dpy);
        wl_registry_add_listener(reg, &reg_listener, NULL);
        wl_display_roundtrip(dpy);
        wl_comp = gdk_wayland_display_get_wl_compositor(gd);
    }
    if (!wl_subcomp || !wl_shm_g) return;

    GtkAllocation a;
    gtk_widget_get_allocation(bar, &a);
    int x, y;
    gtk_widget_translate_coordinates(bar, mus.win, 0, 0, &x, &y);
    wave_w = a.width; wave_h = a.height;
    wave_scale = gtk_widget_get_scale_factor(mus.win);
    for (int i = 0; i < 2; i++)
        if (!shm_buf(&bufs[i], wave_w * wave_scale, wave_h * wave_scale)) return;

    struct wl_surface *parent = gdk_wayland_window_get_wl_surface(gtk_widget_get_window(mus.win));
    wave_surf = wl_compositor_create_surface(wl_comp);
    wl_surface_set_buffer_scale(wave_surf, wave_scale);
    struct wl_region *empty = wl_compositor_create_region(wl_comp);   /* кліки — крізь (на картку, щоб тягнути) */
    wl_surface_set_input_region(wave_surf, empty);
    wl_region_destroy(empty);
    wave_sub = wl_subcompositor_get_subsurface(wl_subcomp, wave_surf, parent);
    wl_subsurface_set_position(wave_sub, x, y);
    wl_subsurface_set_desync(wave_sub);            /* кадри хвилі — незалежно від GTK */
    gtk_widget_queue_draw(mus.win);                /* позиція застосовується з наступним кадром картки */
    wave_draw();
}

/* Смуга зсунулась (фраза в 2–3 рядки замість назви) → перенести хвилю туди ж */
static void on_bar_alloc(GtkWidget *w, GdkRectangle *a, gpointer d) {
    if (!wave_sub) return;
    int x, y;
    if (!gtk_widget_translate_coordinates(bar, mus.win, 0, 0, &x, &y)) return;
    wl_subsurface_set_position(wave_sub, x, y);    /* застосується з наступним кадром картки */
    gtk_widget_queue_draw(mus.win);
}

/* 30 кадрів/с лише поки грає й видно, або поки хвиля випрямляється після паузи */
static gboolean anim_step(gpointer d) {
    double target = playing ? WAVE_AMP : 0;
    amp += (target - amp) * (1 - pow(0.85, 24.0 / WAVE_FPS));   /* плавно до цілі; та сама швидкість за будь-якої частоти кадрів */
    if (fabs(target - amp) < 0.05) amp = target;
    wave_draw();
    if ((!playing && amp == 0) || !visible) return G_SOURCE_REMOVE;   /* завмерло / закрите — стоп */
    return G_SOURCE_CONTINUE;
}
static void anim_start(void) {
    wave_setup();
    if (visible && wave_surf) hw_tick_start(&anim, WAVE_FPS, anim_step);
}

/* ═════════════ НАЗВА: один рядок; не влазить — «…», потім прокручується ═════════════
 * Цикл: MQ_HOLD_S с обрізано «…» → плавно їде вліво до кінця (MQ_SPEED px/с) → MQ_END_S с
 * стоїть → знову з початку. Після MQ_CYCLES циклів — стоїть обрізаною (не гріти даремно);
 * навести мишку — прокрутиться ще раз. Кадри — на спільній сітці (12/с, як хвиля), лише
 * поки картку видно. */
#define MQ_HOLD_S  3.0
#define MQ_END_S   1.5
#define MQ_SPEED   36.0
#define MQ_FPS     12
#define MQ_CYCLES  3
static GtkWidget *title_da;
static char *title_text;
static double mq_off;                  /* зсув, px */
static int mq_phase, mq_cycles;        /* 0 — стоїть «…», 1 — їде, 2 — стоїть у кінці */
static guint mq_wait;
static HwTick mq_tick;

static PangoLayout *title_layout(void) {
    return gtk_widget_create_pango_layout(title_da, title_text ? title_text : "");
}
static int title_overflow(void) {                 /* на скільки px текст ширший за місце (≤0 — влазить) */
    int tw, th;
    PangoLayout *l = title_layout();
    pango_layout_get_pixel_size(l, &tw, &th);
    g_object_unref(l);
    return tw - gtk_widget_get_allocated_width(title_da);
}
static gboolean on_title_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w), tw, th;
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA col;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &col);
    PangoLayout *l = title_layout();
    pango_layout_get_pixel_size(l, &tw, &th);
    if (tw > W && mq_phase == 0) {                 /* не влазить і стоїть — обрізати «…» */
        pango_layout_set_width(l, W * PANGO_SCALE);
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    }
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_clip(cr);
    gdk_cairo_set_source_rgba(cr, &col);
    cairo_move_to(cr, tw > W && mq_phase ? -mq_off : 0, (H - th) / 2.0);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return TRUE;
}
static void mq_stop(void) {
    hw_tick_stop(&mq_tick);
    if (mq_wait) { g_source_remove(mq_wait); mq_wait = 0; }
}
static gboolean mq_step(gpointer d);
static gboolean mq_next(gpointer d) {              /* кінець паузи → наступна фаза */
    mq_wait = 0;
    if (mq_phase == 0) { mq_phase = 1; mq_off = 0; hw_tick_start(&mq_tick, MQ_FPS, mq_step); }
    else {                                         /* постояли в кінці — на початок */
        mq_phase = 0; mq_off = 0;
        gtk_widget_queue_draw(title_da);
        if (++mq_cycles < MQ_CYCLES && visible) mq_wait = g_timeout_add(MQ_HOLD_S * 1000, mq_next, NULL);
    }
    return G_SOURCE_REMOVE;
}
static gboolean mq_step(gpointer d) {
    int over = title_overflow();
    if (!visible || over <= 0) { mq_phase = 0; mq_off = 0; gtk_widget_queue_draw(title_da); return G_SOURCE_REMOVE; }
    mq_off += MQ_SPEED / MQ_FPS;
    if (mq_off >= over) {                          /* доїхали до кінця */
        mq_off = over; mq_phase = 2;
        gtk_widget_queue_draw(title_da);
        mq_wait = g_timeout_add(MQ_END_S * 1000, mq_next, NULL);
        return G_SOURCE_REMOVE;
    }
    gtk_widget_queue_draw(title_da);
    return G_SOURCE_CONTINUE;
}
static void mq_restart(void) {                     /* новий текст / знову видно / навели мишку */
    mq_stop();
    mq_phase = 0; mq_off = 0; mq_cycles = 0;
    gtk_widget_queue_draw(title_da);
    if (visible && gtk_widget_get_mapped(title_da) && title_overflow() > 0)
        mq_wait = g_timeout_add(MQ_HOLD_S * 1000, mq_next, NULL);
}
static void title_set(const char *t) {
    if (!g_strcmp0(t, title_text)) return;
    g_free(title_text);
    title_text = g_strdup(t);
    int tw, th;                                    /* висота — один рядок цього шрифту */
    PangoLayout *l = title_layout();
    pango_layout_get_pixel_size(l, &tw, &th);
    g_object_unref(l);
    gtk_widget_set_size_request(title_da, -1, th);
    gtk_widget_set_tooltip_text(title_da, t);
    mq_restart();
}
static gboolean on_title_enter(GtkWidget *w, GdkEvent *e, gpointer d) { if (mq_phase == 0 && !mq_wait) mq_restart(); return FALSE; }
static void on_title_alloc(GtkWidget *w, GdkRectangle *a, gpointer d) {
    static int last_w;
    if (a->width != last_w) { last_w = a->width; mq_restart(); }
}

/* ═════════════ ФРАЗА: показати / прибрати ═════════════ */
static void quote_show(void) {                     /* назва пісні → фраза (той самий вигляд, один рядок) */
    if (quote_on) title_set(QUOTES[quote_last]);
}
static void quote_set(gboolean on) {
    if (on == quote_on) return;
    quote_on = on;
    if (on) {
        int n = G_N_ELEMENTS(QUOTES), i = g_random_int_range(0, n);
        if (i == quote_last) i = (i + 1) % n;     /* не та сама, що минулого разу */
        quote_last = i;
    }
    quote_show();
}
static gboolean quote_due(gpointer d) { quote_timer = 0; quote_set(TRUE); return G_SOURCE_REMOVE; }
static void quote_update(void) {                   /* після кожного оновлення стану плеєра */
    if (playing) {                                 /* грає — назва пісні */
        if (quote_timer) { g_source_remove(quote_timer); quote_timer = 0; }
        quote_set(FALSE);
    } else if (!player) {                          /* плеєра немає — одразу фраза */
        if (quote_timer) { g_source_remove(quote_timer); quote_timer = 0; }
        quote_set(TRUE);
    } else if (!quote_on && !quote_timer) {        /* пауза — фраза через 30 с (якщо так і не заграє) */
        quote_timer = g_timeout_add_seconds(QUOTE_AFTER_S, quote_due, NULL);
    }
    if (quote_on) quote_show();                    /* refresh щойно переписав назву — повернути фразу */
}

/* ═════════════ МУЗИКА: стан плеєра ═════════════ */
static gboolean update_position(gpointer d) {
    if (!player) return G_SOURCE_CONTINUE;
    GVariant *v = get_prop(player, "Position");
    pos_us = 0;
    if (v) { if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) pos_us = g_variant_get_int64(v); g_variant_unref(v); }
    pos_at = g_get_monotonic_time();
    char a[16], b[16];
    fmt_time(a, sizeof a, pos_us);
    fmt_time(b, sizeof b, length_us);
    gtk_label_set_text(GTK_LABEL(lbl_pos), a);
    gtk_label_set_text(GTK_LABEL(lbl_len), length_us > 0 ? b : "--:--");
    if (!anim.src) wave_draw();                    /* на паузі — оновити смугу один раз */
    return G_SOURCE_CONTINUE;
}

static gboolean refresh(gpointer d) {
    refresh_id = 0;
    g_free(player);
    player = find_active();
    GtkStyleContext *sc = gtk_widget_get_style_context(mus.card);
    length_us = 0;

    if (!player) {                                 /* нічого не грає */
        gtk_style_context_add_class(sc, "idle");
        title_set("Нічого не грає");
        gtk_label_set_text(GTK_LABEL(lbl_artist), "SUPER+A — музика");
        gtk_label_set_text(GTK_LABEL(lbl_pos), "0:00");
        gtk_label_set_text(GTK_LABEL(lbl_len), "--:--");
        playing = FALSE; pos_us = 0;
    } else {
        gtk_style_context_remove_class(sc, "idle");
        GVariant *md = get_prop(player, "Metadata");
        const char *title = NULL;
        char *artists = NULL;
        if (md) {
            GVariant *t = g_variant_lookup_value(md, "xesam:title", G_VARIANT_TYPE_STRING);
            GVariant *a = g_variant_lookup_value(md, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY);
            GVariant *l = g_variant_lookup_value(md, "mpris:length", NULL);
            if (t) title = g_variant_get_string(t, NULL);
            if (a) { const char **arr = g_variant_get_strv(a, NULL); artists = g_strjoinv(", ", (char **)arr); g_free(arr); }
            if (l) { length_us = to_us(l); g_variant_unref(l); }
            title_set(title && *title ? title : "Без назви");
            if (t) g_variant_unref(t);
            if (a) g_variant_unref(a);
            g_variant_unref(md);
        }
        gtk_label_set_text(GTK_LABEL(lbl_artist), artists && *artists ? artists
                           : strstr(player, "kew") ? "Бібліотека" : "YouTube Music");
        g_free(artists);
        playing = is_playing(player);
        update_position(NULL);
    }
    quote_update();                                /* тиша — фраза замість назви пісні */
    anim_start();                                  /* грає — хвиля; пауза — випрямитись і стоп */
    gboolean want = playing && visible;            /* позиція — лише поки грає й видно */
    if (want && !pos_timer) pos_timer = g_timeout_add_seconds(1, update_position, NULL);
    if (!want && pos_timer) { g_source_remove(pos_timer); pos_timer = 0; }
    return G_SOURCE_REMOVE;
}
static void schedule_refresh(void) { if (!refresh_id) refresh_id = g_timeout_add(80, refresh, NULL); }

static void on_props(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                     const char *sig, GVariant *params, gpointer d) { schedule_refresh(); }
static void on_owner(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                     const char *sig, GVariant *params, gpointer d) {
    const char *name;
    g_variant_get(params, "(&sss)", &name, NULL, NULL);
    if (g_str_has_prefix(name, MPRIS)) schedule_refresh();
}

/* ═════════════ МУЗИКА: картка закрита непрозорим вікном → хвиля й позиція стоять ═════════════ */
static HwRect music_rect(HwRect m, gpointer d) {   /* картка прив'язана до правого й верхнього країв */
    double w = gtk_widget_get_allocated_width(mus.win), h = gtk_widget_get_allocated_height(mus.win);
    return (HwRect){ m.x + m.w - mus.shown_right - w, m.y + mus.shown_top, w, h };
}

static gboolean recheck(gpointer d) {
    if (!gtk_widget_get_visible(mus.win)) return G_SOURCE_REMOVE;
    gboolean v = hw_visible(music_rect, NULL);
    if (v != visible) {
        visible = v; refresh(NULL);                /* видно — хвиля й позиція; ні — зупиняться */
        if (v) mq_restart(); else mq_stop();       /* прокрутка назви — теж */
    }
    return G_SOURCE_REMOVE;
}
static void recheck_soon(void) { g_timeout_add(120, recheck, NULL); }

/* Картку показали / сховали (desk.sh toggle music) */
static void music_shown(void) {
    g_timeout_add(300, recheck, NULL);             /* перша перевірка — коли картка вже має розмір */
    schedule_refresh();
}
static void music_hidden(void) {
    mq_stop();
    hw_tick_stop(&anim);
    if (pos_timer) { g_source_remove(pos_timer); pos_timer = 0; }
    if (wave_sub) { wl_subsurface_destroy(wave_sub); wl_surface_destroy(wave_surf); wave_sub = NULL; wave_surf = NULL; frame_pending = FALSE; }
}

void music_init(void) {
    if (!load_pos(&mus)) {                         /* ще не перетягували → під календарем */
        mus.mright = cal.mright;
        mus.mtop = cal.mtop + CAL_H + GRID;
    }
    mus.moved = recheck_soon;                      /* нове місце — може, тепер закрита / відкрита */
    mus.shown = music_shown;
    mus.hidden = music_hidden;
    card_init(&mus, "desktop-music", "music");
    title_da = gtk_drawing_area_new();             /* назва: малюємо самі — щоб прокручувати */
    gtk_widget_add_events(title_da, GDK_ENTER_NOTIFY_MASK);
    g_signal_connect(title_da, "draw", G_CALLBACK(on_title_draw), NULL);
    g_signal_connect(title_da, "enter-notify-event", G_CALLBACK(on_title_enter), NULL);
    g_signal_connect(title_da, "size-allocate", G_CALLBACK(on_title_alloc), NULL);
    lbl_title = title_da;
    lbl_artist = gtk_label_new("");
    add_class(title_da, "title");
    add_class(lbl_artist, "artist");
    for (GtkWidget **l = (GtkWidget *[]){ lbl_artist, NULL }; *l; l++) {
        gtk_label_set_ellipsize(GTK_LABEL(*l), PANGO_ELLIPSIZE_END);   /* довга назва → «…» */
        gtk_label_set_max_width_chars(GTK_LABEL(*l), 1);
        gtk_label_set_xalign(GTK_LABEL(*l), 0);
    }
    bar = gtk_drawing_area_new();                  /* лише місце: малює підповерхня хвилі */
    gtk_widget_set_size_request(bar, -1, BAR_H);
    g_signal_connect(bar, "size-allocate", G_CALLBACK(on_bar_alloc), NULL);
    GtkWidget *times = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    lbl_pos = gtk_label_new("0:00");
    lbl_len = gtk_label_new("--:--");
    add_class(lbl_pos, "time");
    add_class(lbl_len, "time");
    gtk_box_pack_start(GTK_BOX(times), lbl_pos, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(times), lbl_len, FALSE, FALSE, 0);
    for (GtkWidget **w = (GtkWidget *[]){ lbl_title, lbl_artist, bar, times, NULL }; *w; w++)
        gtk_box_pack_start(GTK_BOX(mus.card), *w, FALSE, FALSE, 0);
    for (GtkWidget **w = (GtkWidget *[]){ title_da, bar, NULL }; *w; w++) {   /* полотна не беруть відступи з CSS — як у решти: 8 + 8 */
        gtk_widget_set_margin_start(*w, 8);
        gtk_widget_set_margin_end(*w, 8);
    }

    g_dbus_connection_signal_subscribe(bus, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                       "/org/mpris/MediaPlayer2", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_props, NULL, NULL);
    g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                                       "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_owner, NULL, NULL);
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(mus.win)));
    hw_listen(recheck);
}

