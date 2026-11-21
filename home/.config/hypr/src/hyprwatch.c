/* HYPRWATCH — див. hyprwatch.h */
#include "hyprwatch.h"
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>

/* Програми з прозорим фоном: їхні вікна НЕ закривають віджет */
static const char *SEE_THROUGH[] = { "kitty", NULL };

/* Події, після яких вікна могли зсунутись / з'явитись / зникнути */
static const char *EVENTS[] = { "workspace", "openwindow", "closewindow", "movewindow", "changefloatingmode",
                                "fullscreen", "activespecial", "focusedmon", "moveworkspace", "resizewindow", NULL };

static GSourceFunc on_change[8];                /* кілька віджетів слухають одне з'єднання */
static int n_change;
static guint pending;
static GString *buf;

static gboolean fire(gpointer d) { pending = 0; for (int i = 0; i < n_change; i++) on_change[i](NULL); return G_SOURCE_REMOVE; }

static gboolean on_event(GPollableInputStream *in, gpointer d) {
    char tmp[8192];
    gssize r = g_pollable_input_stream_read_nonblocking(in, tmp, sizeof tmp, NULL, NULL);
    if (r <= 0) return r < 0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    g_string_append_len(buf, tmp, r);
    char *nl;
    while ((nl = memchr(buf->str, '\n', buf->len))) {
        *nl = '\0';
        for (int i = 0; EVENTS[i]; i++)
            if (g_str_has_prefix(buf->str, EVENTS[i])) {
                if (!pending) pending = g_timeout_add(120, fire, NULL);   /* кілька подій поспіль → один виклик */
                break;
            }
        g_string_erase(buf, 0, nl - buf->str + 1);
    }
    return G_SOURCE_CONTINUE;
}

void hw_listen(GSourceFunc cb) {
    if (n_change < (int)G_N_ELEMENTS(on_change)) on_change[n_change++] = cb;
    if (n_change > 1) return;                      /* з'єднання вже є */
    char *path = g_build_filename(g_get_user_runtime_dir(), "hypr", g_getenv("HYPRLAND_INSTANCE_SIGNATURE"),
                                  ".socket2.sock", NULL);
    GSocketClient *client = g_socket_client_new();
    GSocketAddress *addr = g_unix_socket_address_new(path);
    GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
    g_object_unref(addr); g_object_unref(client); g_free(path);
    if (!conn) return;                             /* без подій — віджет просто вважається видимим */
    buf = g_string_new(NULL);
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GSource *src = g_pollable_input_stream_create_source(G_POLLABLE_INPUT_STREAM(in), NULL);
    g_source_set_callback(src, G_SOURCE_FUNC(on_event), NULL, NULL);
    g_source_attach(src, NULL);
    g_source_unref(src);
}

static JsonNode *hyprctl(const char *what) {
    char *out = NULL;
    char *argv[] = { "hyprctl", "-j", (char *)what, NULL };
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL))
        return NULL;
    JsonNode *n = json_from_string(out, NULL);
    g_free(out);
    return n;
}

static gboolean see_through(gint64 pid) {
    char path[64], *comm = NULL;
    g_snprintf(path, sizeof path, "/proc/%" G_GINT64_FORMAT "/comm", pid);
    if (!g_file_get_contents(path, &comm, NULL, NULL)) return FALSE;
    gboolean yes = g_strv_contains(SEE_THROUGH, g_strstrip(comm));
    g_free(comm);
    return yes;
}

static const char *ws_name(JsonObject *o, const char *member) {
    return json_object_get_string_member(json_object_get_object_member(o, member), "name");
}

gboolean hw_visible(HwRectFn rect_fn, gpointer data) {
    JsonNode *mn = hyprctl("monitors"), *cn = hyprctl("clients");
    gboolean vis = TRUE;
    if (!mn || !cn || !JSON_NODE_HOLDS_ARRAY(mn) || !JSON_NODE_HOLDS_ARRAY(cn)) goto out;
    JsonArray *mons = json_node_get_array(mn), *cl = json_node_get_array(cn);
    if (json_array_get_length(mons) == 0) goto out;

    JsonObject *m = json_array_get_object_element(mons, 0);    /* монітор у фокусі */
    for (guint i = 0; i < json_array_get_length(mons); i++) {
        JsonObject *o = json_array_get_object_element(mons, i);
        if (json_object_get_boolean_member_with_default(o, "focused", FALSE)) { m = o; break; }
    }
    const char *ws = ws_name(m, "activeWorkspace"), *sp = ws_name(m, "specialWorkspace");
    double scale = json_object_get_double_member(m, "scale");
    HwRect mon = { json_object_get_int_member(m, "x"), json_object_get_int_member(m, "y"),
                   json_object_get_int_member(m, "width") / scale, json_object_get_int_member(m, "height") / scale };
    HwRect r = rect_fn(mon, data);
    if (g_getenv("HW_DEBUG")) g_printerr("віджет: %.0f,%.0f %.0fx%.0f\n", r.x, r.y, r.w, r.h);

    for (guint i = 0; i < json_array_get_length(cl); i++) {
        JsonObject *c = json_array_get_object_element(cl, i);
        const char *cw = ws_name(c, "workspace");
        if (g_strcmp0(cw, ws) != 0 && !(sp && *sp && g_strcmp0(cw, sp) == 0)) continue;   /* не на екрані */
        if (!json_object_get_boolean_member_with_default(c, "mapped", TRUE) ||
            json_object_get_boolean_member_with_default(c, "hidden", FALSE)) continue;
        if (see_through(json_object_get_int_member(c, "pid"))) continue;                  /* прозоре */
        JsonArray *at = json_object_get_array_member(c, "at"), *sz = json_object_get_array_member(c, "size");
        double x = json_array_get_int_element(at, 0), y = json_array_get_int_element(at, 1);
        double w = json_array_get_int_element(sz, 0), h = json_array_get_int_element(sz, 1);
        if (x < r.x + r.w && x + w > r.x && y < r.y + r.h && y + h > r.y) {
            if (g_getenv("HW_DEBUG")) g_printerr("закриває: %s\n", json_object_get_string_member(c, "class"));
            vis = FALSE;
            break;
        }
    }
out:
    if (mn) json_node_unref(mn);
    if (cn) json_node_unref(cn);
    return vis;
}

/* ═════════════ СПІЛЬНА СІТКА КАДРІВ ═════════════ */
static gboolean tick_step(gpointer d) {
    HwTick *t = d;
    t->src = 0;
    if (!t->cb(NULL)) return G_SOURCE_REMOVE;    /* зупинилась сама */
    hw_tick_start(t, 0, t->cb);                  /* одноразово до наступної точки — не «пливе» */
    return G_SOURCE_REMOVE;
}

void hw_tick_start(HwTick *t, int fps, GSourceFunc cb) {
    if (t->src) return;                          /* уже йде */
    if (fps) t->period = G_USEC_PER_SEC / fps;
    t->cb = cb;
    gint64 now = g_get_monotonic_time();
    guint ms = (guint)(((now / t->period + 1) * t->period - now + 999) / 1000);   /* до наступного k·period */
    t->src = g_timeout_add_full(G_PRIORITY_HIGH, ms, tick_step, t, NULL);
}

void hw_tick_stop(HwTick *t) {
    if (t->src) g_source_remove(t->src);
    t->src = 0;
}

/* ═════════════ ОДНА КОПІЯ + РАННІ СИГНАЛИ ═════════════ */
void hw_single(const char *name, const int *signals) {
    for (const int *s = signals; s && *s; s++) signal(*s, SIG_IGN);   /* до своїх обробників — не вмирати */
    char *path = g_strdup_printf("%s/%s.lock", g_get_user_runtime_dir(), name);
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    g_free(path);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) exit(0);      /* уже працює — друга копія не потрібна */
    /* fd лишається відкритим до кінця програми — замок тримається */
}
