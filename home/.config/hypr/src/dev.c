/* DEV-СТЕК — Docker + робочий проєкт на столі (див. desk.c)
 *
 *   Сервіси й чи слухають вони свої порти: MySQL :3306, PostgreSQL :5432,
 *   Adminer :8080, сайт :8082, адмінка :8083. Клік по веб-сервісу — відкрити в браузері.
 *   Кнопки: «Запустити» / «Зупинити» — усе разом, «Відкрити» — сайт, адмінку й Adminer.
 *   Запуск і зупинка — той самий скрипт, що й раніше: ~/.config/waybar/scripts/devstack.sh
 *   (сповіщення, логи в ~/.cache/devstack).
 *
 *   Стан — з /proc/net/tcp (як ss): Docker НЕ будиться. Раз на 3 с і лише поки
 *   картку видно (закрита непрозорим вікном — стоїть); під час запуску — щосекунди. */
#include "desk.h"

#define SCRIPT ".config/waybar/scripts/devstack.sh"

Card dev = { .pos_name = "deskdev.pos", .off_name = "devstack.off" };

typedef struct { const char *name, *url; int port; GtkWidget *row, *dot, *sub, *state; } Svc;
static Svc SVC[] = {
    { .name = "MySQL", .url = NULL, .port = 3306 },
    { .name = "PostgreSQL", .url = NULL, .port = 5432 },
    { .name = "Adminer", .url = "http://127.0.0.1:8080", .port = 8080 },
    { .name = "Сайт", .url = "http://127.0.0.1:8082", .port = 8082 },
    { .name = "Адмінка", .url = "http://127.0.0.1:8083", .port = 8083 },
};
static GtkWidget *state_lbl, *btn_main, *btn_open;
static guint timer;
static int timer_every;
static gboolean seen;          /* панель висунута й не закрита вікном */

/* Порти, що слухають (стан 0A = LISTEN), з /proc/net/tcp і tcp6 */
static gboolean listening(GHashTable *set, const char *file) {
    char *s = NULL;
    if (!g_file_get_contents(file, &s, NULL, NULL)) return FALSE;
    char **lines = g_strsplit(s, "\n", -1);
    for (char **l = lines + 1; *l; l++) {
        char local[128], rem[128], st[8];
        if (sscanf(*l, "%*d: %127s %127s %7s", local, rem, st) != 3 || strcmp(st, "0A")) continue;
        char *colon = strrchr(local, ':');
        if (colon) g_hash_table_add(set, GINT_TO_POINTER((int)strtol(colon + 1, NULL, 16)));
    }
    g_strfreev(lines); g_free(s);
    return TRUE;
}
static gboolean starting(void) {
    char *f = g_build_filename(g_get_user_cache_dir(), "devstack", "state", NULL);
    gboolean b = g_file_test(f, G_FILE_TEST_EXISTS);
    g_free(f);
    return b;
}

static void retime(void);
static gboolean update(gpointer d) {
    GHashTable *set = g_hash_table_new(g_direct_hash, g_direct_equal);
    listening(set, "/proc/net/tcp");
    listening(set, "/proc/net/tcp6");
    int n_up = 0;
    for (guint i = 0; i < G_N_ELEMENTS(SVC); i++) {
        Svc *s = &SVC[i];
        gboolean up = g_hash_table_contains(set, GINT_TO_POINTER(s->port));
        n_up += up;
        GtkStyleContext *sc = gtk_widget_get_style_context(s->row);
        if (up) gtk_style_context_add_class(sc, "up"); else gtk_style_context_remove_class(sc, "up");
        gtk_label_set_text(GTK_LABEL(s->state), up ? (s->url ? "відкрити" : "працює") : "вимкнено");
        gtk_widget_set_sensitive(s->row, up && s->url);
    }
    g_hash_table_unref(set);

    gboolean busy = starting(), on = n_up > 0;
    GtkStyleContext *sc = gtk_widget_get_style_context(state_lbl);
    for (const char **c = (const char *[]){ "on", "off", "busy", NULL }; *c; c++) gtk_style_context_remove_class(sc, *c);
    gtk_style_context_add_class(sc, busy ? "busy" : on ? "on" : "off");
    gtk_label_set_text(GTK_LABEL(state_lbl), busy ? "запускається…" : n_up == (int)G_N_ELEMENTS(SVC) ? "працює"
                                             : on ? "частково" : "вимкнено");
    gtk_button_set_label(GTK_BUTTON(btn_main), on ? "Зупинити" : "Запустити");
    GtkStyleContext *bc = gtk_widget_get_style_context(btn_main);
    if (on) { gtk_style_context_add_class(bc, "danger"); gtk_style_context_remove_class(bc, "primary"); }   /* зупинити — червона */
    else { gtk_style_context_remove_class(bc, "danger"); gtk_style_context_add_class(bc, "primary"); }
    gtk_widget_set_sensitive(btn_main, !busy);
    gtk_widget_set_sensitive(btn_open, on && !busy);
    drawer_tab_icon(&dev, NULL, on ? "on" : "off");   /* зелений — стек працює */
    if ((busy ? 1 : 3) != timer_every) retime();   /* під час запуску — частіше */
    return G_SOURCE_CONTINUE;
}

static void retime(void) {
    gboolean want = gtk_widget_get_visible(dev.win) && seen;
    int every = starting() ? 1 : 3;
    if (timer && (!want || every != timer_every)) { g_source_remove(timer); timer = 0; }
    if (want && !timer) { timer_every = every; timer = g_timeout_add_seconds(every, update, NULL); update(NULL); }
}
static gboolean recheck(gpointer d) {
    if (gtk_widget_get_visible(dev.win)) seen = card_visible(&dev);
    retime();
    return G_SOURCE_REMOVE;
}
static void recheck_soon(void) { g_timeout_add(150, recheck, NULL); }

static void run_script(const char *arg) {
    char *path = g_build_filename(g_get_home_dir(), SCRIPT, NULL);
    char *argv[] = { path, (char *)arg, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
    g_free(path);
}
static gboolean soon_update(gpointer d) { update(NULL); return G_SOURCE_REMOVE; }
static void on_main(GtkButton *b, gpointer d) {
    run_script("toggle");
    gtk_widget_set_sensitive(btn_main, FALSE);     /* до наступного оновлення */
    g_timeout_add(600, soon_update, NULL);
}
static void on_open(GtkButton *b, gpointer d) { run_script("open"); }
static void on_svc(GtkButton *b, Svc *s) {
    char *argv[] = { "xdg-open", (char *)s->url, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

void dev_init(void) {
    dev.shown = recheck_soon;                      /* висунули / сховали — оновлювати лише поки видно */
    dev.hidden = recheck_soon;
    drawer_card_init(&dev, "devstack", "\xef\x8c\x88", "Dev-стек: Docker + проєкт", TRUE);   /* висувна, язичок унизу */

    GtkWidget *head = ui_header(&dev, "\xef\x8c\x88", "Dev-стек", NULL, NULL);
    state_lbl = gtk_label_new("");
    add_class(state_lbl, "d-state");
    gtk_widget_set_valign(state_lbl, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(head), state_lbl, FALSE, FALSE, 0);
    ui_section(dev.card, "Проєкт");

    for (guint i = 0; i < G_N_ELEMENTS(SVC); i++) {   /* [значок] назва / :порт ······ стан */
        Svc *s = &SVC[i];
        char port[16];
        g_snprintf(port, sizeof port, ":%d", s->port);
        s->row = gtk_button_new();
        add_class(s->row, "d-row");
        gtk_widget_set_can_focus(s->row, FALSE);
        s->dot = ui_lead(s->url ? "\xef\x82\xac" : "\xef\x87\x80");
        add_class(s->dot, "d-kind");
        GtkWidget *box = ui_row_box(s->dot, ui_text(s->name, port, NULL, &s->sub));
        s->state = gtk_label_new("");
        add_class(s->state, "d-sub");
        gtk_widget_set_valign(s->state, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(box), s->state, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(s->row), box);
        if (s->url) g_signal_connect(s->row, "clicked", G_CALLBACK(on_svc), s);
        gtk_box_pack_start(GTK_BOX(dev.card), s->row, FALSE, FALSE, 0);
    }

    GtkWidget *btns = ui_actions(dev.card);
    btn_main = gtk_button_new_with_label("Запустити");
    btn_open = gtk_button_new_with_label("Відкрити");
    add_class(btn_main, "d-btn");
    add_class(btn_main, "main");
    add_class(btn_open, "d-btn");
    g_signal_connect(btn_main, "clicked", G_CALLBACK(on_main), NULL);
    g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open), NULL);
    gtk_box_pack_start(GTK_BOX(btns), btn_main, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(btns), btn_open, TRUE, TRUE, 0);

    gtk_widget_show_all(dev.card);
    update(NULL);
    hw_listen(recheck);
}
