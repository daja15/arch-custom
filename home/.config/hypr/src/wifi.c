/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  WI-FI — висувна панель біля «Сповіщень» (частина desk)      │
 * │  Язичок над Bluetooth; клік — панель висувається, ще клік —  │
 * │  ховається. Угорі: назва, стан словами й перемикач (поки     │
 * │  вмикається / вимикається — неактивний). Під ним — мережі    │
 * │  поруч: сила сигналу, назва, «підключено / збережена /       │
 * │  захищена / відкрита»; більше 7 — прокрутка.                 │
 * │  Клік по мережі: підключена — відключити; збережена —        │
 * │  підключити; відкрита — підключити; захищена нова — поле     │
 * │  пароля (Enter — підключити, Esc — скасувати).               │
 * │  Джерело — NetworkManager (nmcli). Стан і значок на язичку — │
 * │  за подіями NetworkManager (шина D-Bus), без опитування;     │
 * │  список мереж оновлюється раз на 10 с лише поки панель       │
 * │  висунута й видна (нове сканування — не частіше, ніж раз     │
 * │  на 30 с: --rescan auto).                                    │
 * │  Прибрати язичок — desk.sh toggle wifi                       │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include "desk.h"

#define LIST_ROWS 7            /* більше мереж — прокрутка */
#define TICK_S    10           /* поки панель видно — оновлення списку */
#define PEND_S    15           /* увімкнення / вимкнення без відповіді довше — невдача */

Card wfi = { .off_name = "wifi.off" };

typedef struct { char *ssid; int signal; gboolean secure, active, saved; } Net;

static GtkWidget *power_sw, *state_lbl, *list, *scroll, *none, *status;
static GtkWidget *pass_box, *pass_entry;
static char *pass_ssid;        /* вводимо пароль до цієї мережі */
static char *busy_ssid;        /* підключаємось / відключаємось */
static char *active_conn;      /* назва активного з'єднання Wi-Fi */
static gboolean radio_on, sw_updating, seen, connecting;
static int pend_power = -1;    /* чекаємо: 1 — увімкнення, 0 — вимкнення */
static guint tick_src, pend_src, status_src, head_src;
static GHashTable *saved;      /* назви збережених з'єднань Wi-Fi */

/* ───── запуск nmcli без очікування: результат — у колбек */
typedef void (*Done)(const char *out, gboolean ok, gpointer d);
typedef struct { Done cb; gpointer d; } Job;
static void job_done(GObject *o, GAsyncResult *res, gpointer p) {
    Job *j = p;
    char *out = NULL, *err = NULL;
    GError *e = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(o), res, &out, &err, &e) && g_subprocess_get_successful(G_SUBPROCESS(o));
    if (j->cb) j->cb(ok ? (out ? out : "") : err && *err ? err : e ? e->message : "", ok, j->d);
    g_free(out); g_free(err); g_clear_error(&e);
    g_object_unref(o); g_free(j);
}
static void run(Done cb, gpointer d, const char *const *argv) {
    GError *e = NULL;
    GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);
    if (!p) { if (cb) cb(e ? e->message : "", FALSE, d); g_clear_error(&e); return; }
    Job *j = g_new0(Job, 1);
    j->cb = cb; j->d = d;
    g_subprocess_communicate_utf8_async(p, NULL, NULL, job_done, j);
}

/* Рядок «nmcli -t»: поля через «:», «\:» — двокрапка всередині назви */
static char **fields(const char *line, int n) {
    char **f = g_new0(char *, n + 1);
    GString *cur = g_string_new(NULL);
    int k = 0;
    for (const char *p = line; ; p++) {
        if (*p == '\\' && p[1]) { g_string_append_c(cur, *++p); continue; }
        if (*p == ':' || !*p) {
            if (k < n) f[k++] = g_strdup(cur->str);
            g_string_truncate(cur, 0);
            if (!*p) break;
            continue;
        }
        g_string_append_c(cur, *p);
    }
    while (k < n) f[k++] = g_strdup("");
    g_string_free(cur, TRUE);
    return f;
}

/* ───── повідомлення про помилку під списком (зникає саме) */
static gboolean status_hide(gpointer d) { status_src = 0; gtk_widget_hide(status); return G_SOURCE_REMOVE; }
static void say(const char *msg) {
    char *m = g_strstrip(g_strdup(msg));
    char *nl = strchr(m, '\n');
    if (nl) *nl = 0;                               /* лише перший рядок */
    const char *t = g_str_has_prefix(m, "Error: ") ? m + 7 : m;
    gtk_label_set_text(GTK_LABEL(status), t);
    gtk_widget_show(status);
    g_free(m);
    if (status_src) g_source_remove(status_src);
    status_src = g_timeout_add_seconds(8, status_hide, NULL);
}

/* ───── заголовок: стан словами, перемикач, значок на язичку */
static void head_set(const char *dev_state) {
    if (pend_power >= 0 && radio_on == pend_power) { pend_power = -1; if (pend_src) { g_source_remove(pend_src); pend_src = 0; } }
    gboolean conn = radio_on && g_str_has_prefix(dev_state, "connected") && active_conn;
    connecting = radio_on && g_str_has_prefix(dev_state, "connecting");
    sw_updating = TRUE;
    gboolean to = pend_power >= 0 ? pend_power : radio_on;          /* перемикач — куди йдемо */
    gtk_switch_set_active(GTK_SWITCH(power_sw), to);
    gtk_switch_set_state(GTK_SWITCH(power_sw), to);
    sw_updating = FALSE;
    gtk_widget_set_sensitive(power_sw, pend_power < 0);
    char *st = pend_power == 1 ? g_strdup("Вмикається…") : pend_power == 0 ? g_strdup("Вимикається…")
             : !radio_on ? g_strdup("Вимкнено")
             : conn ? g_strdup_printf("Підключено · %s", active_conn)
             : connecting ? g_strdup("Підключаюсь…") : g_strdup("Увімкнено · не підключено");
    gtk_label_set_text(GTK_LABEL(state_lbl), st);
    g_free(st);
    drawer_tab_icon(&wfi, !radio_on ? "\U000F05AA" : conn ? "\U000F05A9" : "\U000F092F", radio_on ? "on" : "off");   /* 󰖪 / 󰖩 / 󰤯; зелений — увімкнено */
}
static void got_dev(const char *out, gboolean ok, gpointer d) {
    char *state = g_strdup("");
    g_clear_pointer(&active_conn, g_free);
    char **lines = g_strsplit(out, "\n", -1);
    for (char **l = lines; *l; l++) {              /* TYPE:STATE:CONNECTION */
        char **f = fields(*l, 3);
        if (!strcmp(f[0], "wifi")) {
            g_free(state); state = g_strdup(f[1]);
            if (*f[2]) active_conn = g_strdup(f[2]);
        }
        g_strfreev(f);
        if (*state) break;
    }
    g_strfreev(lines);
    head_set(state);
    g_free(state);
}
static void got_radio(const char *out, gboolean ok, gpointer d) {
    radio_on = ok && g_str_has_prefix(out, "enabled");
    run(got_dev, NULL, (const char *[]){ "nmcli", "-t", "-f", "TYPE,STATE,CONNECTION", "device", NULL });
}
static gboolean head_refresh(gpointer d) {
    head_src = 0;
    run(got_radio, NULL, (const char *[]){ "nmcli", "-t", "radio", "wifi", NULL });
    return G_SOURCE_REMOVE;
}

/* ───── список мереж */
static void list_refresh(void);
static void pass_close(void) {
    if (!pass_box) return;
    gtk_widget_destroy(pass_box);
    pass_box = pass_entry = NULL;
    g_clear_pointer(&pass_ssid, g_free);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(wfi.win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    ui_list_fit(scroll, list, LIST_ROWS);
}
static void act_done(const char *out, gboolean ok, gpointer d) {
    if (!ok) say(out);
    g_clear_pointer(&busy_ssid, g_free);
    head_refresh(NULL);
    list_refresh();
}
static void connect_new(const char *ssid, const char *pass) {
    g_free(busy_ssid); busy_ssid = g_strdup(ssid);
    if (pass) run(act_done, NULL, (const char *[]){ "nmcli", "device", "wifi", "connect", ssid, "password", pass, NULL });
    else      run(act_done, NULL, (const char *[]){ "nmcli", "device", "wifi", "connect", ssid, NULL });
    list_refresh();
}
static void on_pass_enter(GtkEntry *e, gpointer d) {
    const char *p = gtk_entry_get_text(e);
    if (!*p) return;
    char *ssid = g_strdup(pass_ssid), *pw = g_strdup(p);
    pass_close();
    connect_new(ssid, pw);
    memset(pw, 0, strlen(pw));                     /* пароль у пам'яті не лишати */
    g_free(pw); g_free(ssid);
}
static gboolean on_pass_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    if (e->keyval == GDK_KEY_Escape) { pass_close(); return TRUE; }
    return FALSE;
}
static void pass_open(GtkWidget *row, const char *ssid) {
    pass_close();
    pass_ssid = g_strdup(ssid);
    pass_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    pass_entry = gtk_entry_new();
    add_class(pass_entry, "wf-pass");
    gtk_entry_set_visibility(GTK_ENTRY(pass_entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(pass_entry), GTK_INPUT_PURPOSE_PASSWORD);
    char *ph = g_strdup_printf("Пароль · Enter — підключити, Esc — ні");
    gtk_entry_set_placeholder_text(GTK_ENTRY(pass_entry), ph);
    g_free(ph);
    gtk_widget_set_margin_start(pass_entry, 8);
    gtk_widget_set_margin_end(pass_entry, 8);
    gtk_widget_set_margin_bottom(pass_entry, 4);
    g_signal_connect(pass_entry, "activate", G_CALLBACK(on_pass_enter), NULL);
    g_signal_connect(pass_entry, "key-press-event", G_CALLBACK(on_pass_key), NULL);
    gtk_box_pack_start(GTK_BOX(pass_box), pass_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(list), pass_box, FALSE, FALSE, 0);
    int pos = 0;
    gtk_container_child_get(GTK_CONTAINER(list), row, "position", &pos, NULL);
    gtk_box_reorder_child(GTK_BOX(list), pass_box, pos + 1);   /* під рядком мережі */
    gtk_widget_show_all(pass_box);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(wfi.win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    gtk_widget_grab_focus(pass_entry);
    ui_list_fit(scroll, list, LIST_ROWS + 1);
}
static void on_net(GtkButton *b, gpointer d) {
    if (busy_ssid) return;
    const char *ssid = g_object_get_data(G_OBJECT(b), "ssid");
    gboolean active = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "active"));
    gboolean secure = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "secure"));
    if (pass_ssid && !g_strcmp0(pass_ssid, ssid)) { pass_close(); return; }   /* ще клік — сховати поле */
    if (active && active_conn) {
        busy_ssid = g_strdup(ssid);
        run(act_done, NULL, (const char *[]){ "nmcli", "connection", "down", "id", active_conn, NULL });
        list_refresh();
    } else if (g_hash_table_contains(saved, ssid)) {
        busy_ssid = g_strdup(ssid);
        run(act_done, NULL, (const char *[]){ "nmcli", "connection", "up", "id", ssid, NULL });
        list_refresh();
    } else if (!secure) connect_new(ssid, NULL);
    else pass_open(GTK_WIDGET(b), ssid);
}

static const char *sig_icon(int s) {
    return s >= 75 ? "\U000F0928" : s >= 50 ? "\U000F0925" : s >= 25 ? "\U000F0922" : "\U000F091F";   /* 󰤨 󰤥 󰤢 󰤟 */
}
static gint by_net(gconstpointer a, gconstpointer b) {
    const Net *x = *(Net *const *)a, *y = *(Net *const *)b;
    if (x->active != y->active) return y->active - x->active;   /* підключена — перша */
    if (x->saved != y->saved) return y->saved - x->saved;       /* далі — збережені */
    return y->signal - x->signal;
}
static void net_free(gpointer p) { Net *n = p; g_free(n->ssid); g_free(n); }

static void got_list(const char *out, gboolean ok, gpointer d) {
    if (pass_ssid) return;                         /* вводимо пароль — список не чіпати */
    GHashTable *by = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *nets = g_ptr_array_new_with_free_func(net_free);
    char **lines = g_strsplit(ok ? out : "", "\n", -1);
    for (char **l = lines; *l; l++) {              /* IN-USE:SSID:SIGNAL:SECURITY */
        if (!**l) continue;
        char **f = fields(*l, 4);
        if (*f[1]) {                               /* приховані мережі (без назви) — ні */
            Net *n = g_hash_table_lookup(by, f[1]);
            if (!n) {
                n = g_new0(Net, 1);
                n->ssid = g_strdup(f[1]);
                n->saved = g_hash_table_contains(saved, f[1]);
                g_hash_table_insert(by, n->ssid, n);
                g_ptr_array_add(nets, n);
            }
            n->signal = MAX(n->signal, atoi(f[2]));
            n->active |= !strcmp(f[0], "*");
            n->secure |= *f[3] && strcmp(f[3], "--");
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_hash_table_unref(by);
    g_ptr_array_sort(nets, by_net);

    GList *kids = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *k = kids; k; k = k->next) gtk_widget_destroy(k->data);
    g_list_free(kids);
    for (guint i = 0; i < nets->len; i++) {        /* [сигнал] назва / стан ······ [замок] */
        Net *n = nets->pdata[i];
        gboolean busy = busy_ssid && !strcmp(busy_ssid, n->ssid);
        char *sub = g_strdup_printf("%s · %d%%", busy ? "зачекай…" : n->active ? "підключено" : n->saved ? "збережена"
                                                 : n->secure ? "захищена" : "відкрита", n->signal);
        GtkWidget *s = NULL;
        GtkWidget *b = gtk_button_new();
        add_class(b, "wf-row");
        gtk_widget_set_can_focus(b, FALSE);
        GtkWidget *box = ui_row_box(ui_lead(sig_icon(n->signal)), ui_text(n->ssid, sub, NULL, &s));
        add_class(s, "wf-sub");
        g_free(sub);
        if (n->secure && !n->active && !n->saved) {
            GtkWidget *lock = gtk_label_new("\U000F033E");                  /* 󰌾 потрібен пароль */
            add_class(lock, "wf-lock");
            gtk_widget_set_valign(lock, GTK_ALIGN_CENTER);
            gtk_box_pack_end(GTK_BOX(box), lock, FALSE, FALSE, 0);
        }
        gtk_container_add(GTK_CONTAINER(b), box);
        if (n->active) add_class(b, "conn");
        gtk_widget_set_tooltip_text(b, n->active ? "Клік — відключитись" : n->saved || !n->secure ? "Клік — підключитись"
                                                 : "Клік — ввести пароль і підключитись");
        g_object_set_data_full(G_OBJECT(b), "ssid", g_strdup(n->ssid), g_free);
        g_object_set_data(G_OBJECT(b), "active", GINT_TO_POINTER(n->active));
        g_object_set_data(G_OBJECT(b), "secure", GINT_TO_POINTER(n->secure));
        g_signal_connect(b, "clicked", G_CALLBACK(on_net), NULL);
        gtk_box_pack_start(GTK_BOX(list), b, FALSE, FALSE, 0);
        gtk_widget_show_all(b);
    }
    gtk_label_set_text(GTK_LABEL(none), !radio_on ? "Wi-Fi вимкнено" : "Мереж поруч не знайдено");
    gtk_widget_set_visible(none, nets->len == 0);
    gtk_widget_set_visible(scroll, nets->len > 0);
    g_ptr_array_unref(nets);
    ui_list_fit(scroll, list, LIST_ROWS);
    gtk_window_resize(GTK_WINDOW(wfi.win), 1, 1);
}
static void got_saved(const char *out, gboolean ok, gpointer d) {
    g_hash_table_remove_all(saved);
    char **lines = g_strsplit(ok ? out : "", "\n", -1);
    for (char **l = lines; *l; l++) {              /* NAME:TYPE */
        char **f = fields(*l, 2);
        if (!strcmp(f[1], "802-11-wireless")) g_hash_table_add(saved, g_strdup(f[0]));
        g_strfreev(f);
    }
    g_strfreev(lines);
    if (!radio_on) { got_list("", TRUE, NULL); return; }
    run(got_list, NULL, (const char *[]){ "nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list",
                                          "--rescan", GPOINTER_TO_INT(d) ? "yes" : "auto", NULL });
}
static void list_refresh_full(gboolean rescan) {
    run(got_saved, GINT_TO_POINTER(rescan), (const char *[]){ "nmcli", "-t", "-f", "NAME,TYPE", "connection", "show", NULL });
}
static void list_refresh(void) { list_refresh_full(FALSE); }

/* ───── оновлення лише поки панель висунута й видна */
static gboolean tick(gpointer d) { list_refresh(); return G_SOURCE_CONTINUE; }
static void retime(void) {
    if (tick_src && !seen) { g_source_remove(tick_src); tick_src = 0; }
    if (seen && !tick_src) { tick_src = g_timeout_add_seconds(TICK_S, tick, NULL); list_refresh(); }
}
static gboolean recheck(gpointer d) { seen = card_visible(&wfi); retime(); return G_SOURCE_REMOVE; }
static void on_shown(void) { seen = card_visible(&wfi); if (seen) list_refresh_full(TRUE); retime(); }
static void on_hidden(void) { pass_close(); seen = FALSE; retime(); }

/* ───── перемикач */
static gboolean pend_timeout(gpointer d) {
    pend_src = 0;
    if (pend_power >= 0) say(pend_power ? "Не вдалося увімкнути Wi-Fi" : "Не вдалося вимкнути Wi-Fi");
    pend_power = -1;
    head_refresh(NULL);
    return G_SOURCE_REMOVE;
}
static void power_done(const char *out, gboolean ok, gpointer d) {
    if (!ok) { say(out); pend_power = -1; }
    head_refresh(NULL);
    if (seen) list_refresh();
}
static gboolean on_power(GtkSwitch *s, gboolean on, gpointer d) {
    if (sw_updating || pend_power >= 0) return TRUE;
    pend_power = on;
    head_set("");                                  /* «Вмикається…», перемикач неактивний */
    if (pend_src) g_source_remove(pend_src);
    pend_src = g_timeout_add_seconds(PEND_S, pend_timeout, NULL);
    run(power_done, NULL, (const char *[]){ "nmcli", "radio", "wifi", on ? "on" : "off", NULL });
    return TRUE;
}

/* ───── події NetworkManager: стан змінився → перечитати (раз на 300 мс, не частіше) */
static void on_nm(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *sig, GVariant *p, gpointer d) {
    if (!head_src) head_src = g_timeout_add(300, head_refresh, NULL);
    if (seen && !busy_ssid) list_refresh();
}

void wifi_init(void) {
    saved = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    wfi.shown = on_shown;
    wfi.hidden = on_hidden;
    drawer_card_init(&wfi, "wifi", "\U000F05A9", "Wi-Fi", FALSE);   /* висувна, язичок згори (над Bluetooth) */

    /* [значок] Wi-Fi                 [перемикач]
     *          стан словами
     *  мережі поруч (більше 7 — прокрутка)                                */
    GtkWidget *head = ui_header(&wfi, "\U000F05A9", "Wi-Fi", NULL, &state_lbl);   /* 󰖩 */
    power_sw = gtk_switch_new();
    gtk_widget_set_can_focus(power_sw, FALSE);
    gtk_widget_set_valign(power_sw, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(power_sw, "Увімкнути / вимкнути Wi-Fi");
    g_signal_connect(power_sw, "state-set", G_CALLBACK(on_power), NULL);
    gtk_box_pack_end(GTK_BOX(head), power_sw, FALSE, FALSE, 0);

    none = gtk_label_new("");
    add_class(none, "ui-empty");
    add_class(none, "t-caption");
    gtk_label_set_xalign(GTK_LABEL(none), 0);
    gtk_widget_set_no_show_all(none, TRUE);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll), TRUE);
    add_class(scroll, "ui-list");
    list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(scroll), list);
    status = gtk_label_new("");
    add_class(status, "wf-status");
    gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(status), 0);
    gtk_widget_set_no_show_all(status, TRUE);
    gtk_box_pack_start(GTK_BOX(wfi.card), none, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wfi.card), scroll, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wfi.card), status, FALSE, FALSE, 0);
    gtk_widget_show_all(wfi.card);

    GDBusConnection *sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (sys) {
        g_dbus_connection_signal_subscribe(sys, "org.freedesktop.NetworkManager", "org.freedesktop.NetworkManager", "StateChanged",
                                           "/org/freedesktop/NetworkManager", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_nm, NULL, NULL);
        g_dbus_connection_signal_subscribe(sys, "org.freedesktop.NetworkManager", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                           "/org/freedesktop/NetworkManager", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_nm, NULL, NULL);
    }
    head_refresh(NULL);
    hw_listen(recheck);
}
