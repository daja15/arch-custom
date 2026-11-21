/* ІГРИ — останні ігри Steam і ігри на цьому ПК (див. desk.c)
 *
 *   • «На цьому ПК» — програми з категорією Game (.desktop): Racing 8 DX… (без PortProton);
 *   • «Steam» — 5 останніх запущених ігор акаунта: коли грав і скільки годин
 *     (з userdata/<id>/config/localconfig.vdf, назви — з appmanifest_*.acf бібліотек Steam);
 *   • клік — запустити. Ігри Steam запускаються через клієнт Steam тихо, без вікна
 *     (steam -silent steam://rungameid);
 *     якщо його немає — сповіщення, як встановити.
 *   Оновлюється раз на 10 хвилин і коли картку показали — більше нічого не робить. */
#include "desk.h"
#include <gio/gdesktopappinfo.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>

#define STEAM_MAX  3

Card gms = { .pos_name = "deskgames.pos", .off_name = "games.off" };

typedef struct { int id; gint64 last; int minutes; char *name; } SteamGame;
static GtkWidget *list;

/* Назви для ігор, яких немає в жодній бібліотеці на диску */
static const struct { int id; const char *name; } KNOWN[] = {
    { 570, "Dota 2" }, { 730, "Counter-Strike 2" }, { 440, "Team Fortress 2" }, { 1172470, "Apex Legends" },
    { 230410, "Warframe" }, { 578080, "PUBG: BATTLEGROUNDS" }, { 1085660, "Destiny 2" }, { 236390, "War Thunder" },
};

static const char *tok(const char *s, char *out, size_t n);
static char *read_kv(const char *path, const char *key) {    /* перше «"key" "значення"» у VDF/ACF */
    char *txt = NULL, *res = NULL, t[1024], prev[1024] = "";
    if (!g_file_get_contents(path, &txt, NULL, NULL)) return NULL;
    const char *s = txt;
    while (!res && (s = tok(s, t, sizeof t))) {
        if (!g_ascii_strcasecmp(prev, key) && strcmp(t, "{") && strcmp(t, "}")) res = g_strdup(t);
        g_strlcpy(prev, t, sizeof prev);
    }
    g_free(txt);
    return res;
}
static void names_from_steamapps(GHashTable *h, const char *dirpath) {   /* appmanifest_<id>.acf → "name" */
    GDir *d = g_dir_open(dirpath, 0, NULL);
    const char *e;
    while (d && (e = g_dir_read_name(d))) {
        if (!g_str_has_prefix(e, "appmanifest_") || !g_str_has_suffix(e, ".acf")) continue;
        int id = atoi(e + 12);
        char *p = g_build_filename(dirpath, e, NULL), *n = read_kv(p, "name");
        if (id && n && *n) g_hash_table_replace(h, GINT_TO_POINTER(id), n); else g_free(n);
        g_free(p);
    }
    if (d) g_dir_close(d);
}
static void names_from_libraries(GHashTable *h) {
    /* бібліотеки клієнта Steam (libraryfolders.vdf: усі "path") + теки steamcmd у ~/Games/steam/<гра> */
    char *root = g_build_filename(g_get_user_data_dir(), "Steam", "steamapps", NULL);
    names_from_steamapps(h, root);
    char *lf = g_build_filename(root, "libraryfolders.vdf", NULL), *txt = NULL;
    if (g_file_get_contents(lf, &txt, NULL, NULL)) {
        const char *s = txt; char t[1024], prev[1024] = "";
        while ((s = tok(s, t, sizeof t))) {
            if (!strcmp(prev, "path") && strcmp(t, "{")) {
                char *sa = g_build_filename(t, "steamapps", NULL);
                names_from_steamapps(h, sa); g_free(sa);
            }
            g_strlcpy(prev, t, sizeof prev);
        }
        g_free(txt);
    }
    g_free(lf); g_free(root);
    char *gd = g_build_filename(g_get_home_dir(), "Games", "steam", NULL);
    GDir *d = g_dir_open(gd, 0, NULL);
    const char *e;
    while (d && (e = g_dir_read_name(d))) {
        char *sa = g_build_filename(gd, e, "steamapps", NULL);
        names_from_steamapps(h, sa); g_free(sa);
    }
    if (d) g_dir_close(d);
    g_free(gd);
}

static GHashTable *load_names(void) {             /* id → назва (лише Game / Application, без DLC) */
    GHashTable *h = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    for (guint i = 0; i < G_N_ELEMENTS(KNOWN); i++) g_hash_table_insert(h, GINT_TO_POINTER(KNOWN[i].id), g_strdup(KNOWN[i].name));
    names_from_libraries(h);
    char *f = g_build_filename(g_get_user_cache_dir(), "steam-tui", "games.json", NULL);   /* старий кеш, якщо лишився */
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_file(p, f, NULL) && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(p))) {
        JsonArray *a = json_node_get_array(json_parser_get_root(p));
        for (guint i = 0; i < json_array_get_length(a); i++) {
            JsonObject *o = json_array_get_object_element(a, i);
            const char *type = json_object_get_string_member_with_default(o, "game_type", "");
            if (strcmp(type, "Game") && strcmp(type, "Application")) continue;
            g_hash_table_insert(h, GINT_TO_POINTER((int)json_object_get_int_member_with_default(o, "id", 0)),
                                g_strdup(json_object_get_string_member_with_default(o, "name", "?")));
        }
    }
    g_object_unref(p); g_free(f);
    return h;
}

/* Найпростіший розбір VDF: «"ключ" "значення"» і «"ключ" { … }». Шукаємо блок "apps"
 * (Software/Valve/Steam/apps): у ньому для кожного id — LastPlayed і Playtime (хвилини). */
static const char *tok(const char *s, char *out, size_t n) {   /* наступний токен: рядок у лапках, { або } */
    while (*s && *s != '"' && *s != '{' && *s != '}') s++;
    if (!*s) return NULL;
    if (*s != '"') { out[0] = *s; out[1] = 0; return s + 1; }
    size_t k = 0;
    for (s++; *s && *s != '"'; s++) { if (*s == '\\' && s[1]) s++; if (k + 1 < n) out[k++] = *s; }
    out[k] = 0;
    return *s ? s + 1 : NULL;
}
static void parse_localconfig(const char *path, GHashTable *games) {
    char *txt = NULL;
    if (!g_file_get_contents(path, &txt, NULL, NULL)) return;
    const char *s = txt;
    char t[256], prev[256] = "";
    int depth = 0, apps_depth = -1, cur = 0;
    while ((s = tok(s, t, sizeof t))) {
        if (!strcmp(t, "{")) {
            depth++;
            if (apps_depth < 0 && !g_ascii_strcasecmp(prev, "apps")) apps_depth = depth;
            else if (apps_depth > 0 && depth == apps_depth + 1) cur = atoi(prev);
            prev[0] = 0;
            continue;
        }
        if (!strcmp(t, "}")) {
            if (apps_depth > 0 && depth == apps_depth) break;          /* кінець блоку apps */
            if (apps_depth > 0 && depth == apps_depth + 1) cur = 0;
            depth--; prev[0] = 0;
            continue;
        }
        if (prev[0] && cur && apps_depth > 0 && depth == apps_depth + 1) {   /* пара «ключ значення» всередині гри */
            SteamGame *g = g_hash_table_lookup(games, GINT_TO_POINTER(cur));
            if (!g) { g = g_new0(SteamGame, 1); g->id = cur; g_hash_table_insert(games, GINT_TO_POINTER(cur), g); }
            if (!strcmp(prev, "LastPlayed")) g->last = MAX(g->last, g_ascii_strtoll(t, NULL, 10));
            else if (!strcmp(prev, "Playtime")) g->minutes = MAX(g->minutes, atoi(t));
            prev[0] = 0;
            continue;
        }
        g_strlcpy(prev, t, sizeof prev);
    }
    g_free(txt);
}

static void game_free(gpointer p) { SteamGame *g = p; g_free(g->name); g_free(g); }
static gint by_last(gconstpointer a, gconstpointer b) {
    gint64 x = (*(SteamGame **)a)->last, y = (*(SteamGame **)b)->last;
    return x < y ? 1 : x > y ? -1 : 0;
}

static char *ago(gint64 t) {
    gint64 days = (g_get_real_time() / G_USEC_PER_SEC - t) / 86400;
    if (days <= 0) return g_strdup("сьогодні");
    if (days == 1) return g_strdup("вчора");
    if (days < 14) return g_strdup_printf("%d дн. тому", (int)days);
    if (days < 60) return g_strdup_printf("%d тиж. тому", (int)(days / 7));
    if (days < 365) return g_strdup_printf("%d міс. тому", (int)(days / 30));
    return g_strdup_printf("%d р. тому", (int)(days / 365));
}

/* ── запуск ── */
static void on_steam(GtkButton *b, gpointer d) {
    int id = GPOINTER_TO_INT(d);
    char *steam = g_find_program_in_path("steam");
    if (steam) {
        char *uri = g_strdup_printf("steam://rungameid/%d", id);
        char *argv[] = { steam, "-silent", uri, NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
        g_free(uri);
    } else {
        char *argv[] = { "notify-send", "-a", "Ігри", "Steam не встановлено",
                         "Щоб запускати ігри Steam: sudo pacman -S steam", NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    g_free(steam);
}
static void on_local(GtkButton *b, gpointer d) { g_app_info_launch(G_APP_INFO(d), NULL, NULL, NULL); }

static GtkWidget *row_new(const char *icon, const char *name, const char *sub) {
    /* один рядок: [значок] назва ············ підпис */
    GtkWidget *btn = gtk_button_new();
    add_class(btn, "g-row");
    add_class(btn, "ui-row-dense");
    gtk_widget_set_can_focus(btn, FALSE);
    GtkWidget *box = ui_row_box(ui_lead(icon), ui_text(name, NULL, NULL, NULL));
    if (sub && *sub) {
        GtkWidget *s = gtk_label_new(sub);
        add_class(s, "g-sub");
        gtk_widget_set_valign(s, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(box), s, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(btn), box);
    gtk_widget_set_tooltip_text(btn, name);
    gtk_box_pack_start(GTK_BOX(list), btn, FALSE, FALSE, 0);
    return btn;
}

static gboolean rebuild(gpointer d) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(kids);

    /* На цьому ПК */
    GList *apps = g_app_info_get_all();
    gboolean any_local = FALSE;
    for (GList *l = apps; l; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data) || !g_app_info_should_show(l->data)) continue;
        const char *cat = g_desktop_app_info_get_categories(l->data);
        if (!cat || !strstr(cat, "Game")) continue;
        const char *aid = g_app_info_get_id(l->data), *an = g_app_info_get_display_name(l->data);
        if ((aid && strcasestr(aid, "portproton")) || (an && strcasestr(an, "portproton"))) continue;   /* PortProton — лаунчер, не гра */
        any_local = TRUE;                          /* без розділів: значок геймпада — на цьому ПК, Steam — Steam */
        const char *gn = g_desktop_app_info_get_generic_name(l->data);
        GtkWidget *b = row_new("\xef\x84\x9b", g_app_info_get_display_name(l->data), NULL);
        if (gn) gtk_widget_set_tooltip_text(b, gn);
        g_signal_connect_data(b, "clicked", G_CALLBACK(on_local), g_object_ref(l->data), (GClosureNotify)(void (*)(void))g_object_unref, 0);
    }
    g_list_free_full(apps, g_object_unref);

    /* Steam: усі акаунти на цьому ПК (клієнт — ~/.local/share/Steam, steamcmd — ~/.steam) */
    GHashTable *games = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, game_free);
    char *uds[2] = { g_build_filename(g_get_user_data_dir(), "Steam", "userdata", NULL),
                     g_build_filename(g_get_home_dir(), ".steam", "userdata", NULL) };
    for (int u = 0; u < 2; u++) {
        GDir *dir = g_dir_open(uds[u], 0, NULL);
        const char *e;
        while (dir && (e = g_dir_read_name(dir))) {
            char *p = g_build_filename(uds[u], e, "config", "localconfig.vdf", NULL);
            parse_localconfig(p, games);
            g_free(p);
        }
        if (dir) g_dir_close(dir);
        g_free(uds[u]);
    }
    GHashTable *names = load_names();
    GPtrArray *arr = g_ptr_array_new();
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, games);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SteamGame *g = v;
        const char *n = g_hash_table_lookup(names, k);
        if (!n || !g->last) continue;             /* без назви (DLC, інструменти) — пропустити */
        g->name = g_strdup(n);
        g_ptr_array_add(arr, g);
    }
    g_ptr_array_sort(arr, by_last);
    for (guint i = 0; i < MIN(arr->len, STEAM_MAX); i++) {
        SteamGame *g = arr->pdata[i];
        char *when = ago(g->last), *sub;
        if (g->minutes >= 60) sub = g_strdup_printf("%d год", g->minutes / 60);
        else                  sub = g_strdup_printf("%d хв", g->minutes);
        GtkWidget *b = row_new("\xef\x86\xb6", g->name, sub);
        char *tip = g_strdup_printf("%s\nОстанній раз: %s · наіграно %s", g->name, when, sub);
        gtk_widget_set_tooltip_text(b, tip);
        g_free(tip);
        g_signal_connect(b, "clicked", G_CALLBACK(on_steam), GINT_TO_POINTER(g->id));
        g_free(when); g_free(sub);
    }
    if (!any_local && !arr->len) {
        GtkWidget *l = gtk_label_new("Ігор не знайдено");
        add_class(l, "t-caption");
        add_class(l, "ui-empty");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_pack_start(GTK_BOX(list), l, FALSE, FALSE, 0);
    }
    g_ptr_array_unref(arr);
    g_hash_table_unref(names);
    g_hash_table_unref(games);
    gtk_widget_show_all(list);
    if (gms.win) gtk_window_resize(GTK_WINDOW(gms.win), 1, 1);
    return G_SOURCE_CONTINUE;
}
static void games_shown(void) { rebuild(NULL); }

void games_init(void) {
    gms.shown = games_shown;                       /* висунули — перечитати (інакше нічого не робить) */
    drawer_card_init(&gms, "games", "\xef\x84\x9b", "Ігри", TRUE);   /* висувна, нижня група — над Dev-стеком */
    ui_header(&gms, "\xef\x84\x9b", "Ігри", NULL, NULL);
    list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(list, "ui-list");
    gtk_box_pack_start(GTK_BOX(gms.card), list, FALSE, FALSE, 0);
    rebuild(NULL);
    gtk_widget_show_all(gms.card);
}
