/* ГАРЯЧІ КРАЇ — див. desk.c */
#include "desk.h"
#include <json-glib/json-glib.h>

/* ═════════════ ГАРЯЧІ КРАЇ ═════════════ */
#define HOT_W          2      /* невидима смужка правого краю (px): вузька, щоб не заважати клікам */
#define HOT_H          260
#define HIDE_MS        350    /* затримка перед хованням картки, коли мишка пішла */
#define SLIDE_MS       160    /* тривалість висування картки */
#define TOP_H          2      /* смужка вздовж верхнього краю (px) — показує waybar */
#define LEFT_W         2      /* смужка посередині лівого краю — висуває «Музику» */
#define LEFT_H         300
#define LEFT_DWELL_MS  250    /* скільки тримати курсор біля лівого краю — щоб не висувалась випадково */

/* Програми картки «Зв'язок» (як у меню панелі, waybar/comms.jsonc): виклик, іконка, назва, підпис */
static const struct { const char *lua, *icon, *name, *sub; gboolean add; } COMMS[] = {
    { "CommsShow('nchat')", "", "nchat",         "Telegram",           FALSE },
    { "CommsShow('mail')",  "", "Пошта",         "Gmail · ukr.net",    FALSE },
    { "CommsShow('discord')", "", "Discord", "endcord", FALSE },
    { "CommsAddAccount()",  "", "Додати акаунт", "Telegram / WhatsApp", TRUE },
};

static GtkWidget *comms_win, *revealer, *catcher, *top_win, *left_win;
static gboolean fs_now = FALSE;                /* на поточному столі вікно на весь екран (гра) — краї вимкнено */
static const char *owner = "comms";     /* чия панель відкрита: comms | music — від неї «дірки» й що ховати */
static gboolean comms_enabled = TRUE;          /* панель «Зв'язок» схована → край працює */
static guint hide_timer, dwell_timer;


/* Прозоре вікно-шар без клавіатури; прив'язка до країв: рядок з «t», «b», «l», «r» */
static GtkWidget *edge_window(const char *ns, GtkLayerShellLayer layer, const char *anchors, int zone) {
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GdkVisual *vis = gdk_screen_get_rgba_visual(gdk_screen_get_default());
    if (vis) gtk_widget_set_visual(w, vis);
    gtk_widget_set_app_paintable(w, TRUE);
    gtk_layer_init_for_window(GTK_WINDOW(w));
    gtk_layer_set_namespace(GTK_WINDOW(w), ns);
    gtk_layer_set_layer(GTK_WINDOW(w), layer);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(w), zone);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(w), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    static const struct { char c; GtkLayerShellEdge e; } map[] = {
        { 't', GTK_LAYER_SHELL_EDGE_TOP }, { 'b', GTK_LAYER_SHELL_EDGE_BOTTOM },
        { 'l', GTK_LAYER_SHELL_EDGE_LEFT }, { 'r', GTK_LAYER_SHELL_EDGE_RIGHT } };
    for (guint i = 0; i < G_N_ELEMENTS(map); i++)
        if (strchr(anchors, map[i].c)) gtk_layer_set_anchor(GTK_WINDOW(w), map[i].e, TRUE);
    return w;
}

/* ── Верх: курсор торкнувся → показати waybar (ховає waybarAuto.lua, коли курсор відійде) */
static gboolean top_enter(GtkWidget *w, GdkEvent *e, gpointer d) { hypr_eval("WaybarHover()"); return FALSE; }

/* ── Лівий край: затримав курсор LEFT_DWELL_MS → «Музика» */
static gboolean left_fire(gpointer d) { dwell_timer = 0; hypr_eval("MusicShow()"); return G_SOURCE_REMOVE; }
static gboolean left_enter(GtkWidget *w, GdkEvent *e, gpointer d) {
    if (!dwell_timer) dwell_timer = g_timeout_add(LEFT_DWELL_MS, left_fire, NULL);
    return FALSE;
}
static gboolean left_leave(GtkWidget *w, GdkEvent *e, gpointer d) {
    if (dwell_timer) { g_source_remove(dwell_timer); dwell_timer = 0; }
    return FALSE;
}

/* ── Ловець кліків: на весь екран над вікнами; «дірки» (чат, VLC, меню — пише comms.lua) пропускають кліки */
static void catcher_holes(GtkWidget *w) {
    GdkWindow *gw = gtk_widget_get_window(w);
    if (!gw) return;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    cairo_region_t *r = cairo_region_create_rectangle(&(cairo_rectangle_int_t){ 0, 0, a.width, a.height });
    char *name = g_strdup_printf("%s-holes", owner);   /* comms-holes пише comms.lua, music-holes — music.lua */
    char *path = g_build_filename(g_get_user_runtime_dir(), name, NULL), *txt = NULL;
    g_free(name);
    if (g_file_get_contents(path, &txt, NULL, NULL)) {
        char **lines = g_strsplit(txt, "\n", -1);
        for (char **l = lines; *l; l++) {
            cairo_rectangle_int_t h;
            if (sscanf(*l, "%d %d %d %d", &h.x, &h.y, &h.width, &h.height) == 4) cairo_region_subtract_rectangle(r, &h);
        }
        g_strfreev(lines);
    }
    gdk_window_input_shape_combine_region(gw, r, 0, 0);
    cairo_region_destroy(r); g_free(txt); g_free(path);
}
static gboolean catcher_click(GtkWidget *w, GdkEvent *e, gpointer d) {
    gtk_widget_hide(w);
    hypr_eval(g_strcmp0(owner, "music") == 0 ? "MusicHide()" : "CommsHide()");
    return TRUE;
}

/* ── Правий край: навів — висувається картка; клік — відкрити панель; мишка пішла — сховати */
static gboolean collapse(gpointer d) {
    hide_timer = 0;
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    return G_SOURCE_REMOVE;
}
static void comms_click(GtkButton *b, gpointer lua) { collapse(NULL); hypr_eval(lua); }

static GtkWidget *comms_item(guint i) {
    GtkWidget *b = gtk_button_new(), *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *icon = gtk_label_new(COMMS[i].icon), *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *name = gtk_label_new(COMMS[i].name), *sub = gtk_label_new(COMMS[i].sub);
    gtk_widget_set_size_request(icon, 26, -1);     /* однакова ширина іконок — текст рівно */
    add_class(icon, "icon"); add_class(name, "name"); add_class(sub, "sub");
    gtk_label_set_xalign(GTK_LABEL(name), 0); gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_box_pack_start(GTK_BOX(text), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), text, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), row);
    if (COMMS[i].add) add_class(b, "add");
    gtk_widget_set_can_focus(b, FALSE);
    g_signal_connect(b, "clicked", G_CALLBACK(comms_click), (gpointer)COMMS[i].lua);
    return b;
}

static gboolean comms_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d) {
    if (e->detail == GDK_NOTIFY_INFERIOR) return FALSE;     /* перейшли на кнопку всередині — не вхід */
    if (hide_timer) { g_source_remove(hide_timer); hide_timer = 0; }
    if (comms_enabled) gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
    return FALSE;
}
static gboolean comms_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d) {
    if (e->detail == GDK_NOTIFY_INFERIOR) return FALSE;
    if (hide_timer) g_source_remove(hide_timer);
    hide_timer = g_timeout_add(HIDE_MS, collapse, NULL);
    return FALSE;
}

/* SIGUSR1 — панель висунута: край вимкнено, ловець кліків увімкнено; SIGUSR2 — навпаки */
static gboolean comms_shown(gpointer d) {
    owner = "comms";
    comms_enabled = FALSE;
    collapse(NULL);
    gtk_widget_show_all(catcher);
    catcher_holes(catcher);
    return G_SOURCE_CONTINUE;
}
static gboolean comms_hidden(gpointer d) {
    comms_enabled = TRUE;
    if (g_strcmp0(owner, "comms") == 0) gtk_widget_hide(catcher);
    return G_SOURCE_CONTINUE;
}

/* SIGWINCH — панель «Музика» висунулась / сховалась (music.lua): є файл music-holes — висунута */
static gboolean music_changed(gpointer d) {
    char *f = g_build_filename(g_get_user_runtime_dir(), "music-holes", NULL);
    gboolean open = g_file_test(f, G_FILE_TEST_EXISTS);
    g_free(f);
    if (open) { owner = "music"; gtk_widget_show_all(catcher); catcher_holes(catcher); }
    else if (g_strcmp0(owner, "music") == 0) { gtk_widget_hide(catcher); owner = "comms"; }
    return G_SOURCE_CONTINUE;
}

/* Повний екран (гра, відео) — жодних гарячих країв: курсор біля краю нічого не висуває.
 * Події Hyprland (hw_listen) → hasfullscreen активного столу. «Повний екран у половині»
 * (fullscreen.lua) Hyprland не вважає повним — там краї працюють. */
static gboolean fs_check(gpointer d) {
    char *out = NULL;
    char *argv[] = { "hyprctl", "-j", "activeworkspace", NULL };
    gboolean fs = FALSE;
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL)) {
        JsonNode *n = json_from_string(out, NULL);
        if (n && JSON_NODE_HOLDS_OBJECT(n))
            fs = json_object_get_boolean_member_with_default(json_node_get_object(n), "hasfullscreen", FALSE);
        if (n) json_node_unref(n);
    }
    g_free(out);
    if (fs == fs_now) return G_SOURCE_REMOVE;
    fs_now = fs;
    GtkWidget *ws[] = { top_win, left_win, comms_win };
    for (guint i = 0; i < G_N_ELEMENTS(ws); i++) gtk_widget_set_visible(ws[i], !fs);
    if (fs && dwell_timer) { g_source_remove(dwell_timer); dwell_timer = 0; }
    if (fs) collapse(NULL);
    /* Заокруглені кути екрана (шар поверх усього) — на час гри прибрати: інакше Hyprland
     * складає кожен кадр гри разом з ними, а не віддає гру прямо на екран (direct scanout) */
    if (fs) {
        char *k[] = { "pkill", "-x", "wlr-screen-corn", NULL };     /* comm — 15 символів */
        g_spawn_async(NULL, k, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    } else {
        char *bin = g_build_filename(g_get_home_dir(), ".nix-profile", "bin", "wlr-screen-corners", NULL);
        char *c[] = { "sh", "-c", "pgrep -x wlr-screen-corn >/dev/null || exec \"$0\" --radius 18 --curvature 0", bin, NULL };
        g_spawn_async(NULL, c, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
        g_free(bin);
    }
    return G_SOURCE_REMOVE;
}

void edges_init(void) {
    GtkWidget *top = edge_window("waybar-edge", GTK_LAYER_SHELL_LAYER_OVERLAY, "tlr", -1);   /* над усім */
    gtk_widget_set_size_request(top, -1, TOP_H);
    gtk_widget_add_events(top, GDK_ENTER_NOTIFY_MASK);
    g_signal_connect(top, "enter-notify-event", G_CALLBACK(top_enter), NULL);
    gtk_widget_show_all(top);

    GtkWidget *left = edge_window("music-edge", GTK_LAYER_SHELL_LAYER_OVERLAY, "l", -1);     /* по центру зліва */
    gtk_widget_set_size_request(left, LEFT_W, LEFT_H);
    gtk_widget_add_events(left, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(left, "enter-notify-event", G_CALLBACK(left_enter), NULL);
    g_signal_connect(left, "leave-notify-event", G_CALLBACK(left_leave), NULL);
    gtk_widget_show_all(left);

    catcher = edge_window("comms-catch", GTK_LAYER_SHELL_LAYER_TOP, "tblr", -1);           /* над вікнами, під меню */
    gtk_widget_add_events(catcher, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(catcher, "button-press-event", G_CALLBACK(catcher_click), NULL);
    g_signal_connect(catcher, "size-allocate", G_CALLBACK(catcher_holes), NULL);

    comms_win = edge_window("comms-edge", GTK_LAYER_SHELL_LAYER_OVERLAY, "r", 0);           /* по центру справа */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(box, HOT_W, HOT_H);
    revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), SLIDE_MS);
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(card, "comms");
    GtkWidget *head = gtk_label_new("ЗВ'ЯЗОК");
    add_class(head, "head");
    gtk_label_set_xalign(GTK_LABEL(head), 0);
    gtk_box_pack_start(GTK_BOX(card), head, FALSE, FALSE, 0);
    for (guint i = 0; i < G_N_ELEMENTS(COMMS); i++) gtk_box_pack_start(GTK_BOX(card), comms_item(i), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(revealer), card);
    gtk_box_pack_end(GTK_BOX(box), revealer, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(comms_win), box);
    gtk_widget_add_events(comms_win, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(comms_win, "enter-notify-event", G_CALLBACK(comms_enter), NULL);
    g_signal_connect(comms_win, "leave-notify-event", G_CALLBACK(comms_leave), NULL);
    gtk_widget_show_all(comms_win);

    top_win = top; left_win = left;
    hw_listen(fs_check);
    fs_check(NULL);

    g_unix_signal_add(SIGUSR1, comms_shown, NULL);
    g_unix_signal_add(SIGWINCH, music_changed, NULL);
    g_unix_signal_add(SIGUSR2, comms_hidden, NULL);
}

