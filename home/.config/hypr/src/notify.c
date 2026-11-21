/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  СПОВІЩЕННЯ — віджет робочого столу (частина desk)           │
 * │  Висока картка з історією сповіщень: програма, час,          │
 * │  заголовок, текст. Нові — згори. Клік по сповіщенню —        │
 * │  прибрати, «Очистити» — прибрати всі.                        │
 * │  Як бачимо сповіщення: «слухаємо» шину D-Bus (як             │
 * │  dbus-monitor) — кожен виклик Notify від будь-якої програми. │
 * │  Спливних вікон немає (swaync у режимі «не турбувати», див.  │
 * │  hypr/services.sh) — нове сповіщення лише грає звук (не      │
 * │  частіше ніж раз на секунду; тихі — без звуку) і йде сюди.   │
 * │  Історія зберігається: ~/.cache/desk-notify.json (останні    │
 * │  NOTIFY_MAX) — переживає перезавантаження.                   │
 * │  Дзвіночок у заголовку — «Не турбувати»: сповіщення й далі   │
 * │  приходять сюди, але без звуку (дзвіночок перекреслений).    │
 * │  Пам'ятається: ~/.config/hypr/notify-dnd.                    │
 * │  Сховати / показати — desk.sh toggle notify                  │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include "desk.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>

#define NOTIFY_MAX 100          /* скільки сповіщень пам'ятати */
#define BOTTOM_GAP 60           /* відступ картки від низу екрана */
#define SOUND "/usr/share/sounds/freedesktop/stereo/message-new-instant.oga"   /* звук нового сповіщення */

Card ntf = { .pos_name = "desknotify.pos", .off_name = "notify.off" };

typedef struct { char *app, *icon, *summary, *body; gint64 time; GtkWidget *row; } Note;
static GQueue notes = G_QUEUE_INIT;            /* нові — на початку */
static GtkWidget *box, *count, *empty, *bell, *dnd_lbl;
static gboolean dnd;                            /* «не турбувати» — без звуку */
static char *store_path;

/* ───── збереження */
static void save(void) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_array(b);
    for (GList *l = notes.head; l; l = l->next) {
        Note *n = l->data;
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "app");     json_builder_add_string_value(b, n->app);
        json_builder_set_member_name(b, "icon");    json_builder_add_string_value(b, n->icon);
        json_builder_set_member_name(b, "summary"); json_builder_add_string_value(b, n->summary);
        json_builder_set_member_name(b, "body");    json_builder_add_string_value(b, n->body);
        json_builder_set_member_name(b, "time");    json_builder_add_int_value(b, n->time);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    JsonNode *root = json_builder_get_root(b);
    char *txt = json_to_string(root, FALSE);
    g_file_set_contents(store_path, txt, -1, NULL);
    g_free(txt); json_node_unref(root); g_object_unref(b);
}

static void refresh_count(void) {
    char buf[32];
    g_snprintf(buf, sizeof buf, "%u", notes.length);
    gtk_label_set_text(GTK_LABEL(count), buf);
    gtk_widget_set_visible(count, notes.length > 0);         /* порожню «пігулку» не показувати */
    gtk_widget_set_visible(empty, notes.length == 0);
}

static void note_free(Note *n) { g_free(n->app); g_free(n->icon); g_free(n->summary); g_free(n->body); g_free(n); }

static void note_remove(Note *n) {
    if (n->row) gtk_widget_destroy(n->row);
    g_queue_remove(&notes, n);
    note_free(n);
}

static gboolean on_row_click(GtkWidget *w, GdkEventButton *e, Note *n) {   /* клік — прибрати */
    if (e->button != 1) return FALSE;
    note_remove(n); refresh_count(); save();
    return TRUE;
}

/* Текст сповіщень може містити розмітку (<b>…</b>) — показуємо як звичайний текст */
static char *plain(const char *s) {
    GRegex *re = g_regex_new("<[^>]*>", 0, 0, NULL);
    char *t = g_regex_replace_literal(re, s ? s : "", -1, 0, "", 0, NULL);
    g_regex_unref(re);
    return g_strstrip(t);
}

static void note_show(Note *n, gboolean newest) {
    GtkWidget *ev = gtk_event_box_new();
    gtk_widget_set_tooltip_text(ev, "Клік — прибрати");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    add_class(row, "n-row");
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ic = gtk_image_new_from_icon_name(n->icon && *n->icon ? n->icon : "dialog-information", GTK_ICON_SIZE_MENU);
    GtkWidget *app = gtk_label_new(n->app && *n->app ? n->app : "Сповіщення");
    GDateTime *t = g_date_time_new_from_unix_local(n->time);
    char *ts = g_date_time_format(t, "%H:%M");
    GDateTime *now = g_date_time_new_now_local();
    if (g_date_time_get_day_of_year(now) != g_date_time_get_day_of_year(t)) { g_free(ts); ts = g_date_time_format(t, "%d.%m %H:%M"); }
    GtkWidget *when = gtk_label_new(ts);
    g_free(ts); g_date_time_unref(t); g_date_time_unref(now);
    add_class(app, "n-app"); add_class(when, "n-time");
    gtk_label_set_ellipsize(GTK_LABEL(app), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(head), ic, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), app, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(head), when, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), head, FALSE, FALSE, 0);
    for (int k = 0; k < 2; k++) {
        const char *s = k ? n->body : n->summary;
        if (!s || !*s) continue;
        GtkWidget *l = gtk_label_new(s);
        add_class(l, k ? "n-body" : "n-summary");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
        gtk_label_set_lines(GTK_LABEL(l), k ? 4 : 2);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(ev), row);
    g_signal_connect(ev, "button-press-event", G_CALLBACK(on_row_click), n);
    gtk_box_pack_start(GTK_BOX(box), ev, FALSE, FALSE, 0);
    if (newest) gtk_box_reorder_child(GTK_BOX(box), ev, 0);
    gtk_widget_show_all(ev);
    n->row = ev;
}

static void note_add(char *app, char *icon, char *summary, char *body, gint64 time, gboolean newest, gboolean replace) {
    if (replace && notes.head) {                      /* програма оновлює своє сповіщення — замінити останнє її */
        for (GList *l = notes.head; l; l = l->next)
            if (g_strcmp0(((Note *)l->data)->app, app) == 0) { note_remove(l->data); break; }
    }
    Note *n = g_new0(Note, 1);
    n->app = app; n->icon = icon; n->summary = summary; n->body = body; n->time = time;
    if (newest) g_queue_push_head(&notes, n); else g_queue_push_tail(&notes, n);
    note_show(n, newest);
    while (notes.length > NOTIFY_MAX) note_remove(g_queue_peek_tail(&notes));
}

/* ───── нове сповіщення з шини (у головному потоці) */
typedef struct { char *app, *icon, *summary, *body; gboolean replace, quiet; } Pending;

static void ding(void) {                          /* звук нового сповіщення — не частіше ніж раз на секунду */
    static gint64 last;
    if (dnd) return;                              /* «не турбувати» */
    gint64 now = g_get_monotonic_time();
    if (now - last < G_USEC_PER_SEC) return;
    last = now;
    char *argv[] = { "pw-play", "--volume", "0.7", SOUND, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}
static gboolean deliver(gpointer d) {
    Pending *p = d;
    if (!p->quiet && !p->replace) ding();             /* оновлення того самого сповіщення — без звуку */
    note_add(p->app, p->icon, p->summary, p->body, g_get_real_time() / G_USEC_PER_SEC, TRUE, p->replace);
    g_free(p);
    refresh_count(); save();
    return G_SOURCE_REMOVE;
}

/* Фільтр монітора D-Bus — працює в окремому потоці GDBus: лише копіюємо й передаємо в головний */
static GDBusMessage *on_message(GDBusConnection *c, GDBusMessage *m, gboolean incoming, gpointer d) {
    if (!(g_dbus_message_get_message_type(m) == G_DBUS_MESSAGE_TYPE_METHOD_CALL &&
          g_strcmp0(g_dbus_message_get_member(m), "Notify") == 0 &&
          g_strcmp0(g_dbus_message_get_interface(m), "org.freedesktop.Notifications") == 0))
        return m;                                            /* решту (напр. відповідь на BecomeMonitor) — далі як є */
    {
        GVariant *b = g_dbus_message_get_body(m);
        if (b && g_variant_is_of_type(b, G_VARIANT_TYPE("(susssasa{sv}i)"))) {
            const char *app, *icon, *summary, *body;
            guint32 replaces;
            GVariant *hints;
            g_variant_get(b, "(&su&s&s&s^a&s@a{sv}i)", &app, &replaces, &icon, &summary, &body, NULL, &hints, NULL);
            const char *entry = NULL;                         /* іконка: app_icon, інакше desktop-entry з підказок */
            if (!*icon) g_variant_lookup(hints, "desktop-entry", "&s", &entry);
            Pending *p = g_new0(Pending, 1);
            p->app = g_strdup(app);
            p->icon = g_strdup(*icon ? icon : entry ? entry : "");
            p->summary = plain(summary);
            p->body = plain(body);
            p->replace = replaces != 0;
            guint8 urgency = 1;                                 /* 0 — тихе, 1 — звичайне, 2 — термінове */
            g_variant_lookup(hints, "urgency", "y", &urgency);
            p->quiet = urgency == 0;
            g_variant_unref(hints);
            g_idle_add(deliver, p);
        }
    }
    g_object_unref(m);                                       /* Notify забрали собі */
    return NULL;
}

static void listen_bus(void) {
    char *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    GDBusConnection *mon = addr ? g_dbus_connection_new_for_address_sync(addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL) : NULL;
    g_free(addr);
    if (!mon) return;
    g_dbus_connection_add_filter(mon, on_message, NULL, NULL);
    const char *rules[] = { "type='method_call',interface='org.freedesktop.Notifications',member='Notify'", NULL };
    g_dbus_connection_call_sync(mon, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus.Monitoring",
                                "BecomeMonitor", g_variant_new("(^asu)", rules, 0), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
}

static void load(void) {
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_file(p, store_path, NULL) && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(p))) {
        JsonArray *a = json_node_get_array(json_parser_get_root(p));
        for (guint i = 0; i < json_array_get_length(a) && i < NOTIFY_MAX; i++) {
            JsonObject *o = json_array_get_object_element(a, i);
            note_add(g_strdup(json_object_get_string_member_with_default(o, "app", "")),
                     g_strdup(json_object_get_string_member_with_default(o, "icon", "")),
                     g_strdup(json_object_get_string_member_with_default(o, "summary", "")),
                     g_strdup(json_object_get_string_member_with_default(o, "body", "")),
                     json_object_get_int_member_with_default(o, "time", 0), FALSE, FALSE);
        }
    }
    g_object_unref(p);
}

/* ───── «Не турбувати»: дзвіночок */
static char *dnd_path(void) { return g_build_filename(g_get_home_dir(), ".config/hypr/notify-dnd", NULL); }
static void dnd_show(void) {
    gtk_button_set_label(GTK_BUTTON(bell), dnd ? "\U000F009B" : "\U000F009A");      /* 󰂛 перекреслений / 󰂚 */
    gtk_widget_set_tooltip_text(bell, dnd ? "Не турбувати: сповіщення без звуку\nКлік — увімкнути звук"
                                          : "Клік — «Не турбувати» (сповіщення без звуку)");
    GtkStyleContext *sc = gtk_widget_get_style_context(bell);
    if (dnd) gtk_style_context_add_class(sc, "dnd"); else gtk_style_context_remove_class(sc, "dnd");
    gtk_label_set_text(GTK_LABEL(dnd_lbl), dnd ? "Не турбувати · без звуку" : "Зі звуком");   /* рядок є завжди — нічого не скаче */
    GtkStyleContext *lc = gtk_widget_get_style_context(dnd_lbl);
    if (dnd) gtk_style_context_add_class(lc, "n-dnd"); else gtk_style_context_remove_class(lc, "n-dnd");
}
static void on_bell(GtkButton *b, gpointer d) {
    dnd = !dnd;
    char *f = dnd_path();
    if (dnd) g_file_set_contents(f, "", 0, NULL); else g_unlink(f);
    g_free(f);
    dnd_show();
}

static void clear_all(GtkButton *b, gpointer d) {
    while (notes.head) note_remove(notes.head->data);
    refresh_count(); save();
}

void notify_init(void) {
    store_path = g_build_filename(g_get_user_cache_dir(), "desk-notify.json", NULL);
    if (!load_pos(&ntf)) { ntf.mright = cal.mright + CARD_W + GRID; ntf.mtop = cal.mtop; }   /* ліворуч від календаря */
    card_init(&ntf, "desktop-notify", "notify");

    /* висота — майже весь екран: від верху картки до низу мінус відступ */
    GdkRectangle mon = { 0, 0, 1920, 1080 };
    GdkMonitor *m = gdk_display_get_primary_monitor(gdk_display_get_default());
    if (!m) m = gdk_display_get_monitor(gdk_display_get_default(), 0);
    if (m) gdk_monitor_get_geometry(m, &mon);
    gtk_widget_set_size_request(ntf.card, CARD_W, MAX(300, mon.height - ntf.mtop - BOTTOM_GAP));

    GtkWidget *head = ui_header(&ntf, "\U000F009A", "Сповіщення", NULL, &dnd_lbl);   /* 󰂚 … [число] [Очистити] */
    gtk_label_set_text(GTK_LABEL(dnd_lbl), "Не турбувати · без звуку");
    GList *hk = gtk_container_get_children(GTK_CONTAINER(head));     /* значок → кнопка-дзвіночок */
    gtk_widget_destroy(hk->data);
    g_list_free(hk);
    bell = gtk_button_new_with_label("");
    add_class(bell, "n-bell");
    gtk_widget_set_can_focus(bell, FALSE);
    gtk_widget_set_valign(bell, GTK_ALIGN_CENTER);
    g_signal_connect(bell, "clicked", G_CALLBACK(on_bell), NULL);
    gtk_box_pack_start(GTK_BOX(head), bell, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(head), bell, 0);
    char *df = dnd_path();
    dnd = g_file_test(df, G_FILE_TEST_EXISTS);
    g_free(df);
    count = gtk_label_new("");
    add_class(count, "n-count");
    gtk_widget_set_valign(count, GTK_ALIGN_CENTER);
    GtkWidget *clear = gtk_button_new_with_label("\U000F01B4");          /* 󰆴 очистити */
    add_class(clear, "n-clear");
    gtk_widget_set_tooltip_text(clear, "Очистити все");
    gtk_widget_set_valign(clear, GTK_ALIGN_CENTER);
    gtk_widget_set_can_focus(clear, FALSE);
    g_signal_connect(clear, "clicked", G_CALLBACK(clear_all), NULL);
    gtk_box_pack_end(GTK_BOX(head), clear, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(head), count, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll), TRUE);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    empty = gtk_label_new("Сповіщень немає");
    add_class(empty, "n-empty");
    gtk_label_set_xalign(GTK_LABEL(empty), 0);
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(inner), empty, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), box, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(scroll), inner);
    gtk_box_pack_start(GTK_BOX(ntf.card), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(ntf.win)));
    gtk_widget_set_no_show_all(count, TRUE);      /* видимість — за кількістю (show_all вікна їх не чіпає) */
    gtk_widget_set_no_show_all(empty, TRUE);
    dnd_show();

    load();
    refresh_count();
    listen_bus();
}
