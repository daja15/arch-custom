/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  ПОГОДА — віджет робочого столу (частина desk)               │
 * │  Навпіл, через лінію:                                        │
 * │    ліворуч — зараз: значок, температура, опис, «відчувається │
 * │      як», місце;                                             │
 * │    праворуч — день: макс / мін, опади, схід і захід сонця,   │
 * │      тривалість дня, вологість, вітер, УФ-індекс.            │
 * │  Без значків у тексті — лише назва й значення.               │
 * │  Місце — за IP-адресою (ip-api.com), раз на годину.          │
 * │  Погода — Open-Meteo (безкоштовно, без ключа), кожні 5 хв.   │
 * │  Запити — curl у фоні: віджет не чекає мережі. Немає мережі  │
 * │  — лишаються останні дані з позначкою часу.                  │
 * │  Сховати / показати — desk.sh toggle weather                 │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include "desk.h"
#include <json-glib/json-glib.h>

#define WEATHER_EVERY  (5 * 60)        /* оновлювати погоду, с */
#define PLACE_EVERY    (60 * 60)       /* уточнювати місце, с */
#define GEO_URL  "http://ip-api.com/json/?fields=status,city,country,lat,lon"
#define METEO_URL "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s" \
                  "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day" \
                  "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset,daylight_duration,precipitation_probability_max,uv_index_max" \
                  "&forecast_days=1&wind_speed_unit=ms&timezone=auto"

Card wth = { .pos_name = "deskweather.pos", .off_name = "weather.off", .mright = CAL_RIGHT };

static GtkWidget *l_icon, *l_temp, *l_desc, *l_place, *l_hum, *l_feel, *l_wind, *l_upd;
static GtkWidget *l_maxmin, *l_rain, *l_sunrise, *l_sunset, *l_daylen, *l_uv;
#define WTH_W  420                      /* ширина картки: дві половини */
static double lat, lon;
static gboolean have_place;
static gint64 place_at;

/* WMO-код погоди → опис і іконка Nerd Font (день / ніч) */
static void describe(int code, gboolean day, const char **text, const char **icon) {
    static const struct { int lo, hi; const char *text, *day, *night; } T[] = {
        { 0,  0,  "Ясно",               "\U000F0599", "\U000F0594" },
        { 1,  1,  "Переважно ясно",     "\U000F0595", "\U000F0F31" },
        { 2,  2,  "Мінлива хмарність",  "\U000F0595", "\U000F0F31" },
        { 3,  3,  "Хмарно",             "\U000F0590", "\U000F0590" },
        { 45, 48, "Туман",              "\U000F0591", "\U000F0591" },
        { 51, 57, "Мряка",              "\U000F0597", "\U000F0597" },
        { 61, 65, "Дощ",                "\U000F0597", "\U000F0597" },
        { 66, 67, "Крижаний дощ",       "\U000F067F", "\U000F067F" },
        { 71, 77, "Сніг",               "\U000F0598", "\U000F0598" },
        { 80, 82, "Злива",              "\U000F0596", "\U000F0596" },
        { 85, 86, "Снігопад",           "\U000F0F36", "\U000F0F36" },
        { 95, 99, "Гроза",              "\U000F0593", "\U000F0593" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(T); i++)
        if (code >= T[i].lo && code <= T[i].hi) { *text = T[i].text; *icon = day ? T[i].day : T[i].night; return; }
    *text = "—"; *icon = "\U000F0590";
}

/* curl у фоні: url → cb(JSON-об'єкт або NULL) */
typedef void (*JsonCb)(JsonObject *o);
static void fetched(GObject *src, GAsyncResult *res, gpointer cb) {
    char *out = NULL;
    JsonObject *o = NULL;
    JsonNode *n = NULL;
    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, NULL) && out && *out) {
        n = json_from_string(out, NULL);
        if (n && JSON_NODE_HOLDS_OBJECT(n)) o = json_node_get_object(n);
    }
    ((JsonCb)cb)(o);
    if (n) json_node_unref(n);
    g_free(out);
    g_object_unref(src);
}
static void fetch(const char *url, JsonCb cb) {
    GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                                      "curl", "-sf", "--max-time", "15", url, NULL);
    if (p) g_subprocess_communicate_utf8_async(p, NULL, NULL, fetched, cb);
}

static double round0(double t) { t = round(t); return t == 0 ? 0 : t; }   /* без «−0°» */

static void got_weather(JsonObject *o) {
    if (!o || !json_object_has_member(o, "current")) {                  /* мережі немає — лишаємо старе */
        gtk_label_set_text(GTK_LABEL(l_upd), "немає зв'язку · оновлю пізніше");
        return;
    }
    JsonObject *c = json_object_get_object_member(o, "current");
    const char *text, *icon;
    describe((int)json_object_get_int_member(c, "weather_code"), json_object_get_int_member(c, "is_day"), &text, &icon);
    char buf[64];
    gtk_label_set_text(GTK_LABEL(l_icon), icon);
    g_snprintf(buf, sizeof buf, "%.0f°", round0(json_object_get_double_member(c, "temperature_2m")));
    gtk_label_set_text(GTK_LABEL(l_temp), buf);
    gtk_label_set_text(GTK_LABEL(l_desc), text);
    g_snprintf(buf, sizeof buf, "%d%%", (int)json_object_get_int_member(c, "relative_humidity_2m"));
    gtk_label_set_text(GTK_LABEL(l_hum), buf);
    g_snprintf(buf, sizeof buf, "відчувається як %.0f°", round0(json_object_get_double_member(c, "apparent_temperature")));
    gtk_label_set_text(GTK_LABEL(l_feel), buf);
    g_snprintf(buf, sizeof buf, "%.0f м/с", json_object_get_double_member(c, "wind_speed_10m"));
    gtk_label_set_text(GTK_LABEL(l_wind), buf);
    if (json_object_has_member(o, "daily")) {                           /* день: перші (сьогоднішні) значення */
        JsonObject *dl = json_object_get_object_member(o, "daily");
        #define DAY0(k) (json_object_has_member(dl, k) ? json_array_get_element(json_object_get_array_member(dl, k), 0) : NULL)
        JsonNode *mx = DAY0("temperature_2m_max"), *mn = DAY0("temperature_2m_min"), *pr = DAY0("precipitation_probability_max"),
                 *sr = DAY0("sunrise"), *ss = DAY0("sunset"), *dd = DAY0("daylight_duration"), *uv = DAY0("uv_index_max");
        #undef DAY0
        if (mx && mn) { g_snprintf(buf, sizeof buf, "%.0f° / %.0f°", round0(json_node_get_double(mx)), round0(json_node_get_double(mn))); gtk_label_set_text(GTK_LABEL(l_maxmin), buf); }
        if (pr && !json_node_is_null(pr)) { g_snprintf(buf, sizeof buf, "%d%%", (int)json_node_get_int(pr)); gtk_label_set_text(GTK_LABEL(l_rain), buf); }
        const char *s;                                                   /* «2026-11-17T07:12» → «07:12» */
        if (sr && (s = json_node_get_string(sr)) && strlen(s) >= 16) gtk_label_set_text(GTK_LABEL(l_sunrise), s + 11);
        if (ss && (s = json_node_get_string(ss)) && strlen(s) >= 16) gtk_label_set_text(GTK_LABEL(l_sunset), s + 11);
        if (dd) { int m = (int)(json_node_get_double(dd) / 60); g_snprintf(buf, sizeof buf, "%d год %02d хв", m / 60, m % 60); gtk_label_set_text(GTK_LABEL(l_daylen), buf); }
        if (uv && !json_node_is_null(uv)) {
            double u = json_node_get_double(uv);
            g_snprintf(buf, sizeof buf, "%.0f · %s", u, u < 3 ? "низький" : u < 6 ? "помірний" : u < 8 ? "високий" : "дуже високий");
            gtk_label_set_text(GTK_LABEL(l_uv), buf);
        }
    }
    GDateTime *now = g_date_time_new_now_local();
    char *t = g_date_time_format(now, "оновлено о %H:%M");
    gtk_label_set_text(GTK_LABEL(l_upd), t);
    g_free(t); g_date_time_unref(now);
}

static void update_weather(void) {
    if (!have_place) return;
    char la[G_ASCII_DTOSTR_BUF_SIZE], lo[G_ASCII_DTOSTR_BUF_SIZE];   /* крапка, а не кома (мова системи — українська) */
    char *url = g_strdup_printf(METEO_URL, g_ascii_formatd(la, sizeof la, "%.4f", lat), g_ascii_formatd(lo, sizeof lo, "%.4f", lon));
    fetch(url, got_weather);
    g_free(url);
}

static void got_place(JsonObject *o) {
    if (o && g_strcmp0(json_object_get_string_member_with_default(o, "status", ""), "success") == 0) {
        lat = json_object_get_double_member(o, "lat");
        lon = json_object_get_double_member(o, "lon");
        have_place = TRUE;
        place_at = g_get_monotonic_time();
        char *p = g_strdup_printf("%s, %s", json_object_get_string_member(o, "city"),
                                  json_object_get_string_member(o, "country"));
        gtk_label_set_text(GTK_LABEL(l_place), p);
        g_free(p);
    }
    update_weather();
}

static gboolean tick(gpointer d) {                 /* кожні 5 хв; місце — раз на годину (і поки його немає) */
    if (!have_place || g_get_monotonic_time() - place_at > (gint64)PLACE_EVERY * G_USEC_PER_SEC) fetch(GEO_URL, got_place);
    else update_weather();
    return G_SOURCE_CONTINUE;
}

static GtkWidget *label(const char *cls, const char *text) {
    GtkWidget *l = gtk_label_new(text);
    add_class(l, cls);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

void weather_init(void) {
    if (!load_pos(&wth)) { wth.mright = mus.mright; wth.mtop = mus.mtop + 120 + GRID; }   /* під музикою */
    card_init(&wth, "desktop-weather", "weather");

    gtk_widget_set_size_request(wth.card, WTH_W, -1);
    GtkWidget *halves = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(halves), FALSE);

    /* ── ліворуч: зараз */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(left, TRUE);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    l_icon = label("w-icon", "\U000F0590");
    l_temp = label("w-temp", "--°");
    gtk_box_pack_start(GTK_BOX(top), l_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), l_temp, FALSE, FALSE, 0);
    l_desc = label("w-desc", "отримую погоду…");
    l_feel = label("w-feel", "");
    l_place = label("w-place", "визначаю місце…");
    gtk_label_set_ellipsize(GTK_LABEL(l_place), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l_place), 1);
    l_upd = label("w-upd", "");
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (GtkWidget **x = (GtkWidget *[]){ top, l_desc, l_feel, spacer, l_place, l_upd, NULL }; *x; x++)
        gtk_box_pack_start(GTK_BOX(left), *x, *x == spacer, *x == spacer, 0);

    /* ── лінія */
    GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(sep, "w-sep");

    /* ── праворуч: день — назва ········ значення */
    GtkWidget *right = gtk_grid_new();
    add_class(right, "w-day");
    gtk_grid_set_row_spacing(GTK_GRID(right), 6);
    gtk_grid_set_column_spacing(GTK_GRID(right), 12);
    GtkWidget *head = label("w-dayhead", "Сьогодні");
    gtk_grid_attach(GTK_GRID(right), head, 0, 0, 2, 1);
    struct { const char *name; GtkWidget **val; } R[] = {
        { "Макс / мін", &l_maxmin }, { "Опади", &l_rain }, { "Схід", &l_sunrise }, { "Захід", &l_sunset },
        { "Світловий день", &l_daylen }, { "Вологість", &l_hum }, { "Вітер", &l_wind }, { "УФ-індекс", &l_uv },
    };
    for (guint i = 0; i < G_N_ELEMENTS(R); i++) {
        GtkWidget *n = label("w-name", R[i].name);
        *R[i].val = label("w-val", "—");
        gtk_label_set_xalign(GTK_LABEL(*R[i].val), 1);
        gtk_widget_set_hexpand(*R[i].val, TRUE);
        gtk_grid_attach(GTK_GRID(right), n, 0, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(right), *R[i].val, 1, i + 1, 1, 1);
    }

    gtk_box_pack_start(GTK_BOX(halves), left, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(halves), sep, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(halves), right, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(wth.card), halves, FALSE, FALSE, 0);
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(wth.win)));

    tick(NULL);
    g_timeout_add_seconds(WEATHER_EVERY, tick, NULL);
}
