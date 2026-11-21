/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  BLUETOOTH — віджет робочого столу (частина desk)            │
 * │  Угорі: заголовок і перемикач; під ним — стан словами       │
 * │  («Вмикається…», «Шукаю пристрої…», «Увімкнено · 1 підключено»)│
 * │  і кнопка «Пошук». Поки дія не завершилась (вмикається,      │
 * │  запускається пошук…) — перемикач і кнопка неактивні, стають │
 * │  активними, щойно BlueZ підтвердив новий стан.               │
 * │  Пошук іде, доки його не зупинити: раз на 5 с оновлюється    │
 * │  список і сигнал (і пошук перезапускається, якщо BlueZ його   │
 * │  зупинив сам).                                               │
 * │  Список пристроїв: значок, назва, стан, сила сигналу         │
 * │  (4 рисочки; точне число dBm — у підказці), кнопки:          │
 * │    ✎ — перейменувати (Enter — зберегти, Esc — скасувати,      │
 * │        порожня назва — повернути рідну);                     │
 * │    󰂱 / 󰅖 — підключити (нове — спершу сполучити) / відключити.│
 * │    кошик (лише під час редагування) — забути сполучений      │
 * │        пристрій (клік двічі: перший — «точно?»).             │
 * │  Сполучений пристрій не відповідає — віджет сам шукає його   │
 * │  поруч (30 с): озвався за старою адресою — підключає; поруч, │
 * │  але «забув» сполучення (скинули / сполучили з іншим) або    │
 * │  змінив адресу — забуває старий запис і сполучає заново      │
 * │  (з тією ж назвою, що була).                                 │
 * │  Порядок: спершу підключені, далі — від найсильнішого        │
 * │  сигналу (найближчі) до найслабшого, далі — сполучені, яких  │
 * │  зараз не чути.                                              │
 * │  Сила сигналу (RSSI) відома, лише коли пристрій «кричить»     │
 * │  у ефір — тобто під час пошуку; BlueZ не дає її для вже       │
 * │  підключених без прав root.                                  │
 * │  Чужі пристрої без назви (лише адреса) не показуємо.         │
 * │  Джерело — BlueZ (системна шина D-Bus), лише події.          │
 * │  Висувна панель біля «Сповіщень» (язичок 󰂯); прибрати       │
 * │  язичок — desk.sh toggle bluetooth                           │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include "desk.h"

Card blt = { .pos_name = "deskbt.pos", .off_name = "bluetooth.off" };

#define BZ       "org.bluez"
#define ADAPTER  "org.bluez.Adapter1"
#define DEVICE   "org.bluez.Device1"
#define TICK_S   5             /* під час пошуку — оновлення списку */
#define PEND_S   15            /* дія без відповіді довше — вважати невдалою */
#define LIST_ROWS 7            /* більше пристроїв — прокрутка */

typedef struct {
    char *path;
    GDBusProxy *px;
    GtkWidget *row, *icon, *name, *entry, *sub, *bars[4], *sig, *edit, *act, *del;
    gboolean editing, busy, del_armed;
    guint del_src;
    int rssi;                  /* G_MININT — невідомо */
    int group;                 /* 0 — підключений, 1 — чути, 2 — сполучений, але не чути */
} Row;

static GDBusObjectManager *om;
static GDBusProxy *adapter;
static GtkWidget *power_sw, *scan_btn, *list, *scroll, *status, *none, *state_lbl;
static GHashTable *rows;       /* шлях → Row* */
static guint scan_src, sort_src, status_src, pend_src;
static int pend_power = -1;    /* чекаємо: 1 — увімкнення, 0 — вимкнення, −1 — нічого */
static int pend_scan = -1;     /* чекаємо: 1 — запуск пошуку, 0 — зупинку */
static gboolean sw_updating;
static gint64 scan_t;          /* коли запустили (перші 2 с BlueZ ще може казати «не шукаю») */
void (*bt_say_hook)(const char *msg);   /* картки «підключити» (devconn.c) теж показують повідомлення */
static gboolean own_scan;      /* пошук запустили МИ (міст Switch 2 шукає сам — те не показуємо, щоб не блимало) */

/* «Пошук загубленого» (див. hunt_*): 0 — ні, 1 — шукаю поруч, 2 — озвався, підключаю,
 * 3 — старий запис забуто, чекаю, коли пристрій з'явиться в режимі сполучення */
#define HUNT_S   30
static int hunt_phase;
static char *hunt_path, *hunt_name, *hunt_alias;
static guint hunt_src;
static gboolean hunt_own;      /* пошук ефіру запустили саме для цього — зупинити після */
static void hunt_check(gpointer row);
static void hunt_start(gpointer row);
static void scan_started(GObject *o, GAsyncResult *res, gpointer d);
static void scan_stopped(GObject *o, GAsyncResult *res, gpointer d);
static void pend_start(void);
static void head_update(void);

/* ───── властивості */
static GVariant *prop(GDBusProxy *p, const char *k) { return p ? g_dbus_proxy_get_cached_property(p, k) : NULL; }
static gboolean bln(GDBusProxy *p, const char *k) {
    GVariant *v = prop(p, k);
    gboolean r = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
    if (v) g_variant_unref(v);
    return r;
}
static char *str(GDBusProxy *p, const char *k) {
    GVariant *v = prop(p, k);
    char *r = v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_dup_string(v, NULL) : NULL;
    if (v) g_variant_unref(v);
    return r;
}
static int rssi(GDBusProxy *p) {
    GVariant *v = prop(p, "RSSI");
    int r = v && g_variant_is_of_type(v, G_VARIANT_TYPE_INT16) ? g_variant_get_int16(v) : G_MININT;
    if (v) g_variant_unref(v);
    return r;
}
static void set_prop(GDBusProxy *p, const char *iface, const char *k, GVariant *val,
                     GAsyncReadyCallback cb, gpointer d) {
    g_dbus_proxy_call(p, "org.freedesktop.DBus.Properties.Set", g_variant_new("(ssv)", iface, k, val),
                      G_DBUS_CALL_FLAGS_NONE, 10000, NULL, cb, d);
}

/* ───── рядок стану внизу (помилки) */
static gboolean status_clear(gpointer d) { status_src = 0; gtk_widget_hide(status); return G_SOURCE_REMOVE; }
static void say(const char *msg) {
    if (bt_say_hook) bt_say_hook(msg);
    gtk_label_set_text(GTK_LABEL(status), msg);
    gtk_widget_show(status);
    if (status_src) g_source_remove(status_src);
    status_src = g_timeout_add_seconds(6, status_clear, NULL);
}
static void say_err(const char *what, GError *e) {
    char *m = e ? g_dbus_error_get_remote_error(e) : NULL;
    const char *t = m && strstr(m, "AuthenticationFailed") ? "не прийняв пароль / PIN"
                  : m && strstr(m, "AuthenticationCanceled") ? "скасовано"
                  : m && (strstr(m, "Timeout") || strstr(m, "ConnectionAttemptFailed") || strstr(m, "PageTimeout")) ? "не відповідає (далеко чи вимкнено?)"
                  : e && (strstr(e->message, "abort") || strstr(e->message, "Abort")) ? "не відповідає (вимкнено чи сполучено з іншим пристроєм?)"
                  : m && strstr(m, "AlreadyExists") ? "уже сполучено"
                  : m && strstr(m, "Blocked") ? "заблоковано (rfkill)"
                  : m && strstr(m, "NotReady") ? "Bluetooth вимкнено"
                  : e ? e->message : "невідома помилка";
    char *s = g_strdup_printf("%s: %s", what, t);
    say(s);
    g_free(s); g_free(m);
}


/* ───── агент сполучення. Без нього BlueZ сполучає «без запам'ятовування» (Paired, але не Bonded),
 * і геймпади/клавіатури потім відкидає («Rejected connection from !bonded device»).
 * Наш агент: «немає ні екрана, ні клавіатури» — погоджуємось на все, що просить пристрій,
 * PIN (старі пристрої) — 0000. Діє лише для сполучень, які запустили ми (кнопка в картці). */
#define AGENT_PATH "/org/desk/btagent"
static const char *AGENT_XML =
    "<node><interface name='org.bluez.Agent1'>"
    "<method name='Release'/>"
    "<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
    "<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
    "<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='Cancel'/>"
    "</interface></node>";
static GDBusConnection *agent_bus;
static guint agent_reg;

static void agent_call(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                       const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer d) {
    if (!g_strcmp0(method, "RequestPinCode")) g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "0000"));
    else if (!g_strcmp0(method, "RequestPasskey")) g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 0));
    else if (!g_strcmp0(method, "DisplayPinCode")) {
        const char *pin;
        g_variant_get(params, "(&o&s)", NULL, &pin);
        char *s = g_strdup_printf("Набери на пристрої: %s і Enter", pin);
        say(s); g_free(s);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!g_strcmp0(method, "DisplayPasskey")) {
        guint32 key;
        g_variant_get(params, "(&ouq)", NULL, &key, NULL);
        char *s = g_strdup_printf("Набери на пристрої: %06u і Enter", key);
        say(s); g_free(s);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else g_dbus_method_invocation_return_value(inv, NULL);   /* підтвердження / дозволи — так */
}

static void agent_register(void) {
    if (!agent_bus) return;
    g_dbus_connection_call(agent_bus, BZ, "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
                           g_variant_new("(os)", AGENT_PATH, "NoInputNoOutput"), NULL, G_DBUS_CALL_FLAGS_NONE,
                           5000, NULL, NULL, NULL);
}
static void on_bluez_up(GDBusConnection *c, const char *name, const char *owner, gpointer d) { agent_register(); }   /* і після перезапуску BlueZ */

static void agent_init(void) {
    agent_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (!agent_bus) return;
    GDBusNodeInfo *ni = g_dbus_node_info_new_for_xml(AGENT_XML, NULL);
    static const GDBusInterfaceVTable vt = { agent_call, NULL, NULL, { 0 } };
    agent_reg = g_dbus_connection_register_object(agent_bus, AGENT_PATH, ni->interfaces[0], &vt, NULL, NULL, NULL);
    g_dbus_node_info_unref(ni);
    g_bus_watch_name_on_connection(agent_bus, BZ, G_BUS_NAME_WATCHER_FLAGS_NONE, on_bluez_up, NULL, NULL, NULL);
}

/* ───── значок за типом пристрою (властивість Icon з BlueZ) */
static const char *dev_icon(const char *icon) {
    if (!icon) return "\U000F00AF";
    if (strstr(icon, "headset"))  return "\U000F02CE";
    if (strstr(icon, "audio") || strstr(icon, "headphone")) return "\U000F02CB";
    if (strstr(icon, "speaker"))  return "\U000F04C3";
    if (strstr(icon, "mouse"))    return "\U000F037D";
    if (strstr(icon, "keyboard")) return "\U000F030C";
    if (strstr(icon, "tablet"))   return "\U000F04F6";
    if (strstr(icon, "gaming"))   return "\U000F0297";
    if (strstr(icon, "phone"))    return "\U000F011C";
    if (strstr(icon, "computer")) return "\U000F0322";
    if (strstr(icon, "camera"))   return "\U000F0100";
    if (strstr(icon, "printer"))  return "\U000F042A";
    return "\U000F00AF";
}

/* ───── сортування й показ */
static gint by_rank(gconstpointer a, gconstpointer b) {
    const Row *x = *(Row *const *)a, *y = *(Row *const *)b;
    if (x->group != y->group) return x->group - y->group;
    if (x->rssi != y->rssi) return x->rssi > y->rssi ? -1 : 1;           /* сильніший — вище */
    return g_strcmp0(gtk_label_get_text(GTK_LABEL(x->name)), gtk_label_get_text(GTK_LABEL(y->name)));
}
static gboolean do_sort(gpointer d) {
    sort_src = 0;
    GPtrArray *a = g_ptr_array_new();
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, rows);
    while (g_hash_table_iter_next(&it, &k, &v)) if (((Row *)v)->group >= 0) g_ptr_array_add(a, v);
    g_ptr_array_sort(a, by_rank);
    for (guint i = 0; i < a->len; i++) gtk_box_reorder_child(GTK_BOX(list), ((Row *)a->pdata[i])->row, i);
    gtk_widget_set_visible(none, a->len == 0);
    g_ptr_array_unref(a);
    /* до LIST_ROWS пристроїв — увесь список, більше — прокрутка */
    ui_list_fit(scroll, list, LIST_ROWS);
    gtk_window_resize(GTK_WINDOW(blt.win), 1, 1);
    return G_SOURCE_REMOVE;
}
static void resort(void) { if (!sort_src) sort_src = g_timeout_add(300, do_sort, NULL); }   /* під час пошуку RSSI сиплеться часто */

static void row_update(Row *r) {
    gboolean conn = bln(r->px, "Connected"), paired = bln(r->px, "Paired") || bln(r->px, "Bonded");
    char *name = str(r->px, "Name"), *alias = str(r->px, "Alias"), *icon = str(r->px, "Icon"), *addr = str(r->px, "Address");
    r->rssi = own_scan ? rssi(r->px) : G_MININT;   /* чужий пошук — сигнал не показуємо */
    gboolean heard = r->rssi != G_MININT;
    r->group = conn ? 0 : heard && (name || paired) ? 1 : paired ? 2 : -1;   /* −1 — чужий без назви / не чути */
    gtk_widget_set_visible(r->row, r->group >= 0);
    if (r->group >= 0) {
        gtk_label_set_text(GTK_LABEL(r->icon), dev_icon(icon));
        if (!r->editing) gtk_label_set_text(GTK_LABEL(r->name), alias ? alias : addr ? addr : "?");
        char *tip = g_strdup_printf("%s\n%s%s%s", alias ? alias : "", addr ? addr : "",
                                    name && g_strcmp0(name, alias) ? "\nСправжня назва: " : "", name && g_strcmp0(name, alias) ? name : "");
        gtk_widget_set_tooltip_text(r->name, tip);
        g_free(tip);
        gboolean hunted = hunt_phase && !g_strcmp0(r->path, hunt_path);
        gtk_label_set_text(GTK_LABEL(r->sub), hunted ? (hunt_phase == 2 ? "озвався — підключаю…" : "шукаю поруч…")
                                              : r->busy ? "зачекай…" : conn ? "підключено" : paired ? "сполучено" : "поруч");
        gtk_widget_set_visible(r->del, r->editing && paired);     /* редагування сполученого: кошик замість «підключити» */
        gtk_widget_set_visible(r->act, !(r->editing && paired));
        GtkStyleContext *sc = gtk_widget_get_style_context(r->row);
        if (conn) gtk_style_context_add_class(sc, "conn"); else gtk_style_context_remove_class(sc, "conn");
        /* сила сигналу: ≥ −55 dBm — 4 рисочки, ≥ −67 — 3, ≥ −80 — 2, інакше 1 */
        int n = !heard ? 0 : r->rssi >= -55 ? 4 : r->rssi >= -67 ? 3 : r->rssi >= -80 ? 2 : 1;
        for (int i = 0; i < 4; i++) {
            GtkStyleContext *b = gtk_widget_get_style_context(r->bars[i]);
            if (i < n) gtk_style_context_add_class(b, "on"); else gtk_style_context_remove_class(b, "on");
        }
        char *st = heard ? g_strdup_printf("Сигнал: %d dBm", r->rssi)
                         : g_strdup(conn ? "Сигнал невідомий: BlueZ не дає його для підключених"
                                         : "Сигнал невідомий: натисни «Пошук»");
        gtk_widget_set_tooltip_text(r->sig, st);
        g_free(st);
        gtk_button_set_label(GTK_BUTTON(r->act), conn ? "\U000F0156" : "\U000F00B1");          /* 󰅖 / 󰂱 */
        gtk_widget_set_tooltip_text(r->act, conn ? "Відключити" : paired ? "Підключити" : "Сполучити й підключити");
        gtk_widget_set_sensitive(r->act, !r->busy);
    }
    g_free(name); g_free(alias); g_free(icon); g_free(addr);
    resort();
    hunt_check(r);
}

/* ───── підключити / відключити */
static void connect_done(GObject *o, GAsyncResult *res, gpointer d);
static void connect_dev(Row *r) {
    g_dbus_proxy_call(r->px, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, connect_done, g_strdup(r->path));
}
static Row *row_by(const char *path) { return path ? g_hash_table_lookup(rows, path) : NULL; }
static void paired_done(GObject *o, GAsyncResult *res, gpointer d);
static void pair_dev(Row *r) {
    r->busy = TRUE;
    if (agent_bus)                                 /* через те саме з'єднання, що й агент — інакше BlueZ його не побачить */
        g_dbus_connection_call(agent_bus, BZ, r->path, DEVICE, "Pair", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 60000,
                               NULL, paired_done, g_strdup(r->path));
    else g_dbus_proxy_call(r->px, "Pair", NULL, G_DBUS_CALL_FLAGS_NONE, 60000, NULL, paired_done, g_strdup(r->path));
}
static void forget_dev(const char *path) {
    if (adapter) g_dbus_proxy_call(adapter, "RemoveDevice", g_variant_new("(o)", path), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL, NULL);
}

/* ───── «пошук загубленого» */
static void hunt_stop(const char *msg) {
    if (hunt_src) { g_source_remove(hunt_src); hunt_src = 0; }
    Row *r = row_by(hunt_path);
    hunt_phase = 0;
    g_clear_pointer(&hunt_path, g_free); g_clear_pointer(&hunt_name, g_free); g_clear_pointer(&hunt_alias, g_free);
    if (hunt_own && own_scan && adapter && pend_scan < 0) {
        pend_scan = 0;
        pend_start();
        g_dbus_proxy_call(adapter, "StopDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, scan_stopped, NULL);
    }
    hunt_own = FALSE;
    if (r) { r->busy = FALSE; row_update(r); }
    if (msg) say(msg);
    head_update();
}
static gboolean hunt_timeout(gpointer d) {
    hunt_src = 0;
    hunt_stop(hunt_phase == 3 ? "Не дочекався режиму сполучення. Затисни кнопку сполучення на пристрої й натисни «Пошук»"
                              : "Не знайшов поруч. Увімкни пристрій (або затисни кнопку сполучення) і натисни ще раз");
    return G_SOURCE_REMOVE;
}
static void hunt_start(gpointer row) {
    Row *r = row;
    if (hunt_phase) hunt_stop(NULL);
    hunt_phase = 1;
    hunt_path = g_strdup(r->path);
    hunt_name = str(r->px, "Name");
    hunt_alias = str(r->px, "Alias");
    r->busy = TRUE;
    hunt_src = g_timeout_add_seconds(HUNT_S, hunt_timeout, NULL);
    if (!own_scan && adapter && pend_scan < 0) {   /* сигнал (RSSI) видно лише під час нашого пошуку */
        hunt_own = TRUE;
        pend_scan = 1;
        pend_start();
        scan_t = g_get_monotonic_time();
        g_dbus_proxy_call(adapter, "StartDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, scan_started, NULL);
    }
    char *s = g_strdup_printf("«%s» не відповідає — шукаю поруч (міг змінити адресу)…", hunt_alias ? hunt_alias : "?");
    say(s); g_free(s);
    head_update();
    row_update(r);
}
/* Кожне оновлення рядка: чи це не наш загублений? */
static void hunt_check(gpointer row) {
    Row *r = row;
    if ((hunt_phase != 1 && hunt_phase != 3) || r->rssi == G_MININT || r->busy) return;
    gboolean paired = bln(r->px, "Paired") || bln(r->px, "Bonded");
    if (hunt_phase == 1 && paired && !g_strcmp0(r->path, hunt_path)) {   /* озвався за старою адресою — підключити */
        hunt_phase = 2;
        r->busy = TRUE;
        connect_dev(r);
        row_update(r);
        return;
    }
    if (paired) return;
    char *n = str(r->px, "Name");
    gboolean same = n && hunt_name && !strcmp(n, hunt_name);
    g_free(n);
    if (!same) return;
    /* той самий пристрій, але не сполучений: нова адреса або чекав сполучення — сполучити заново */
    char *old = g_strdup(hunt_path), *alias = g_strdup(hunt_alias);
    gboolean moved = g_strcmp0(old, r->path) != 0;
    hunt_stop(moved ? "Знайшов за новою адресою — сполучаю заново…" : "Знайшов — сполучаю заново…");
    if (moved && row_by(old)) forget_dev(old);     /* старий запис більше не потрібен */
    if (alias && hunt_name && strcmp(alias, hunt_name)) set_prop(r->px, DEVICE, "Alias", g_variant_new_string(alias), NULL, NULL);
    g_free(old); g_free(alias);
    pair_dev(r);
    row_update(r);
}

static void connect_done(GObject *o, GAsyncResult *res, gpointer d) {
    GError *e = NULL;
    GVariant *v = g_dbus_proxy_call_finish(G_DBUS_PROXY(o), res, &e);
    Row *r = row_by(d);
    gboolean hunted = hunt_phase && !g_strcmp0(d, hunt_path);
    g_free(d);
    if (v) g_variant_unref(v);
    if (!r) { g_clear_error(&e); return; }
    r->busy = FALSE;
    if (!e) { if (hunted) hunt_stop(NULL); row_update(r); return; }
    gboolean paired = bln(r->px, "Paired") || bln(r->px, "Bonded");
    if (hunted && hunt_phase == 2) {
        /* поруч, але не пускає: на пристрої вже нема цього сполучення (скинули / сполучили з іншим) */
        hunt_phase = 3;
        if (hunt_src) g_source_remove(hunt_src);
        hunt_src = g_timeout_add_seconds(HUNT_S + 15, hunt_timeout, NULL);
        char *s = g_strdup_printf("«%s» поруч, але забув сполучення. Затисни на ньому кнопку сполучення — сполучу заново",
                                  hunt_alias ? hunt_alias : "?");
        say(s); g_free(s);
        forget_dev(r->path);                       /* з'явиться знову вже несполученим — hunt_check сполучить */
    } else if (!hunt_phase && paired && !(e->message && strstr(e->message, "InProgress"))) hunt_start(r);
    else say_err(gtk_label_get_text(GTK_LABEL(r->name)), e);
    g_clear_error(&e);
    if (row_by(r->path)) row_update(r);
}
static void act_done(GObject *o, GAsyncResult *res, gpointer d) {
    GError *e = NULL;
    GVariant *v = g_dbus_proxy_call_finish(G_DBUS_PROXY(o), res, &e);
    Row *r = row_by(d);                            /* рядок міг зникнути, поки чекали */
    g_free(d);
    if (v) g_variant_unref(v);
    if (!r) { g_clear_error(&e); return; }
    r->busy = FALSE;
    if (e) say_err(gtk_label_get_text(GTK_LABEL(r->name)), e);
    g_clear_error(&e);
    row_update(r);
}
static void paired_done(GObject *o, GAsyncResult *res, gpointer d) {
    GError *e = NULL;
    GVariant *v = G_IS_DBUS_PROXY(o) ? g_dbus_proxy_call_finish(G_DBUS_PROXY(o), res, &e)
                                     : g_dbus_connection_call_finish(G_DBUS_CONNECTION(o), res, &e);
    Row *r = row_by(d);
    if (v) g_variant_unref(v);
    if (!r) { g_free(d); g_clear_error(&e); return; }
    if (e && !strstr(e->message, "AlreadyExists")) {
        g_free(d);
        r->busy = FALSE;
        say_err(gtk_label_get_text(GTK_LABEL(r->name)), e);
        g_error_free(e);
        row_update(r);
        return;
    }
    g_clear_error(&e);
    g_free(d);
    set_prop(r->px, DEVICE, "Trusted", g_variant_new_boolean(TRUE), NULL, NULL);   /* далі — підключатись без питань */
    connect_dev(r);
}
static void on_act(GtkButton *b, Row *r) {
    if (r->busy) return;
    r->busy = TRUE;
    if (bln(r->px, "Connected"))
        g_dbus_proxy_call(r->px, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, 15000, NULL, act_done, g_strdup(r->path));
    else if (bln(r->px, "Paired") || bln(r->px, "Bonded")) connect_dev(r);
    else pair_dev(r);
    row_update(r);
}

/* ───── перейменування. Картка — у шарі «фон» без клавіатури; на час редагування — клавіатура «на вимогу» */
static void edit_end(Row *r, gboolean save) {
    if (!r->editing) return;
    r->editing = FALSE;
    if (save) {
        const char *t = gtk_entry_get_text(GTK_ENTRY(r->entry));
        char *s = g_strstrip(g_strdup(t));
        set_prop(r->px, DEVICE, "Alias", g_variant_new_string(s), NULL, NULL);   /* "" — повернути рідну назву */
        if (*s) gtk_label_set_text(GTK_LABEL(r->name), s);
        g_free(s);
    }
    gtk_widget_hide(r->entry);
    gtk_widget_show(r->name);
    gtk_button_set_label(GTK_BUTTON(r->edit), "\U000F03EB");
    gboolean any = FALSE;
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, rows);
    while (g_hash_table_iter_next(&it, &k, &v)) if (((Row *)v)->editing) any = TRUE;
    if (!any) gtk_layer_set_keyboard_mode(GTK_WINDOW(blt.win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    row_update(r);
}
static void on_edit(GtkButton *b, Row *r) {
    if (r->editing) { edit_end(r, TRUE); return; }
    r->editing = TRUE;
    gtk_layer_set_keyboard_mode(GTK_WINDOW(blt.win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    gtk_entry_set_text(GTK_ENTRY(r->entry), gtk_label_get_text(GTK_LABEL(r->name)));
    gtk_widget_hide(r->name);
    gtk_widget_show(r->entry);
    gtk_widget_grab_focus(r->entry);
    gtk_editable_select_region(GTK_EDITABLE(r->entry), 0, -1);
    gtk_button_set_label(GTK_BUTTON(r->edit), "\U000F012C");             /* 󰄬 зберегти */
    row_update(r);                                                         /* кошик «забути» — для сполучених */
}
static void on_entry_enter(GtkEntry *e, Row *r) { edit_end(r, TRUE); }

/* ───── забути пристрій (кошик — лише під час редагування; перший клік — «точно?», другий — забути) */
static void del_disarm(Row *r) {
    r->del_armed = FALSE;
    if (r->del_src) { g_source_remove(r->del_src); r->del_src = 0; }
    gtk_style_context_remove_class(gtk_widget_get_style_context(r->del), "danger");
    gtk_widget_set_tooltip_text(r->del, "Забути пристрій (прибрати зі списку сполучених)");
}
static gboolean del_timeout(gpointer d) { Row *r = d; r->del_src = 0; del_disarm(r); return G_SOURCE_REMOVE; }
static void on_del(GtkButton *b, Row *r) {
    if (!r->del_armed) {
        r->del_armed = TRUE;
        gtk_style_context_add_class(gtk_widget_get_style_context(r->del), "danger");
        gtk_widget_set_tooltip_text(r->del, "Натисни ще раз — забути");
        r->del_src = g_timeout_add_seconds(3, del_timeout, r);
        return;
    }
    del_disarm(r);
    if (hunt_phase && !g_strcmp0(r->path, hunt_path)) hunt_stop(NULL);
    char *path = g_strdup(r->path), *nm = g_strdup(gtk_label_get_text(GTK_LABEL(r->name)));
    edit_end(r, FALSE);
    forget_dev(path);                              /* рядок зникне сам (BlueZ прибере пристрій) */
    char *s = g_strdup_printf("«%s» забуто", nm);
    say(s);
    g_free(s); g_free(nm); g_free(path);
}
static gboolean on_entry_key(GtkWidget *w, GdkEventKey *e, Row *r) {
    if (e->keyval == GDK_KEY_Escape) { edit_end(r, FALSE); return TRUE; }
    return FALSE;
}

/* ───── рядки */
static GtkWidget *small_btn(const char *label, const char *tip) {
    GtkWidget *b = gtk_button_new_with_label(label);
    add_class(b, "bt-btn");
    gtk_widget_set_can_focus(b, FALSE);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    return b;
}

static void row_free(gpointer x) {
    Row *r = x;
    if (r->editing) { r->editing = FALSE; gtk_layer_set_keyboard_mode(GTK_WINDOW(blt.win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE); }
    if (r->del_src) g_source_remove(r->del_src);
    gtk_widget_destroy(r->row);
    g_object_unref(r->px);
    g_free(r->path); g_free(r);
}

static void row_add(GDBusObject *obj) {
    GDBusInterface *i = g_dbus_object_get_interface(obj, DEVICE);
    if (!i) return;
    const char *path = g_dbus_object_get_object_path(obj);
    if (g_hash_table_contains(rows, path)) { g_object_unref(i); return; }
    Row *r = g_new0(Row, 1);
    r->path = g_strdup(path);
    r->px = G_DBUS_PROXY(i);
    r->row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);   /* [значок] 12 [назва / стан] ··· [сигнал] [✎] [⏻] */
    add_class(r->row, "bt-row");
    r->icon = ui_lead("");
    add_class(r->icon, "bt-icon");
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(mid, 8);                  /* 4 (проміжок рядка) + 8 = 12 від значка */
    gtk_widget_set_valign(mid, GTK_ALIGN_CENTER);
    r->name = gtk_label_new("");
    add_class(r->name, "bt-name");
    gtk_label_set_ellipsize(GTK_LABEL(r->name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(r->name), 1);
    gtk_label_set_xalign(GTK_LABEL(r->name), 0);
    gtk_widget_set_has_tooltip(r->name, TRUE);
    r->entry = gtk_entry_new();
    add_class(r->entry, "bt-entry");
    gtk_entry_set_max_length(GTK_ENTRY(r->entry), 248);
    gtk_entry_set_width_chars(GTK_ENTRY(r->entry), 8);
    gtk_entry_set_placeholder_text(GTK_ENTRY(r->entry), "порожньо — рідна назва");
    g_signal_connect(r->entry, "activate", G_CALLBACK(on_entry_enter), r);
    g_signal_connect(r->entry, "key-press-event", G_CALLBACK(on_entry_key), r);
    r->sub = gtk_label_new("");
    add_class(r->sub, "bt-sub");
    gtk_label_set_xalign(GTK_LABEL(r->sub), 0);
    gtk_box_pack_start(GTK_BOX(mid), r->name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mid), r->entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mid), r->sub, FALSE, FALSE, 0);

    r->sig = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);                 /* 4 рисочки сили сигналу */
    add_class(r->sig, "bt-sig");
    gtk_widget_set_valign(r->sig, GTK_ALIGN_CENTER);
    for (int k = 0; k < 4; k++) {
        r->bars[k] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        add_class(r->bars[k], "s-bar");
        gtk_widget_set_size_request(r->bars[k], 3, 4 + k * 3);
        gtk_widget_set_valign(r->bars[k], GTK_ALIGN_END);
        gtk_box_pack_start(GTK_BOX(r->sig), r->bars[k], FALSE, FALSE, 0);
    }
    GtkWidget *sig_ev = gtk_event_box_new();                              /* підказка над рисочками */
    gtk_container_add(GTK_CONTAINER(sig_ev), r->sig);
    gtk_widget_set_size_request(sig_ev, 24, 16);
    gtk_widget_set_margin_end(sig_ev, 4);
    gtk_widget_set_valign(sig_ev, GTK_ALIGN_CENTER);
    r->edit = small_btn("\U000F03EB", "Перейменувати");                  /* 󰏫 */
    r->act = small_btn("\U000F00B1", NULL);
    r->del = small_btn("\U000F0A7A", "Забути пристрій (прибрати зі списку сполучених)");   /* кошик */
    g_signal_connect(r->edit, "clicked", G_CALLBACK(on_edit), r);
    g_signal_connect(r->act, "clicked", G_CALLBACK(on_act), r);
    g_signal_connect(r->del, "clicked", G_CALLBACK(on_del), r);

    gtk_box_pack_start(GTK_BOX(r->row), r->icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(r->row), mid, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(r->row), sig_ev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(r->row), r->edit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(r->row), r->act, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(r->row), r->del, FALSE, FALSE, 0);   /* на місці «підключити» */
    gtk_widget_show_all(r->row);
    gtk_widget_hide(r->entry);
    gtk_widget_hide(r->del);
    gtk_widget_set_no_show_all(r->del, TRUE);
    gtk_widget_set_no_show_all(r->act, TRUE);
    gtk_widget_set_no_show_all(r->row, TRUE);      /* видимість — лише наша */
    gtk_widget_set_no_show_all(r->entry, TRUE);
    gtk_box_pack_start(GTK_BOX(list), r->row, FALSE, FALSE, 0);
    g_hash_table_insert(rows, r->path, r);
    row_update(r);
}

/* ───── адаптер: перемикач і пошук */
static void rows_update_all(void);
static void head_update(void);
static gboolean pend_timeout(gpointer d) {        /* BlueZ так і не відповів */
    pend_src = 0;
    if (pend_power >= 0) say(pend_power ? "Не вдалося увімкнути" : "Не вдалося вимкнути");
    else if (pend_scan >= 0) say("Пошук не відповідає");
    pend_power = pend_scan = -1;
    head_update();
    return G_SOURCE_REMOVE;
}
static void pend_start(void) { if (pend_src) g_source_remove(pend_src); pend_src = g_timeout_add_seconds(PEND_S, pend_timeout, NULL); }
static void pend_check(void) { if (pend_power < 0 && pend_scan < 0 && pend_src) { g_source_remove(pend_src); pend_src = 0; } }

static int n_connected(void) {
    int n = 0;
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, rows);
    while (g_hash_table_iter_next(&it, &k, &v)) n += bln(((Row *)v)->px, "Connected");
    return n;
}

static void head_update(void) {
    gboolean on = adapter && bln(adapter, "Powered");
    if (pend_power >= 0 && (on == pend_power || (!adapter && !pend_power))) pend_power = -1;   /* BlueZ підтвердив */
    if (!on && own_scan && pend_power < 0) { own_scan = FALSE; pend_scan = -1; }           /* вимкнули — пошуку немає */
    pend_check();
    gboolean busy = pend_power >= 0 || pend_scan >= 0;
    sw_updating = TRUE;
    gboolean shown = pend_power >= 0 ? pend_power : on;                                     /* перемикач — куди йдемо */
    gtk_switch_set_active(GTK_SWITCH(power_sw), shown);
    gtk_switch_set_state(GTK_SWITCH(power_sw), shown);
    sw_updating = FALSE;
    gtk_widget_set_sensitive(power_sw, adapter != NULL && pend_power < 0);
    gtk_widget_set_sensitive(scan_btn, on && !busy);
    gtk_button_set_label(GTK_BUTTON(scan_btn), pend_scan == 1 ? "Запускаю…" : pend_scan == 0 ? "Зупиняю…"
                                              : own_scan ? "Зупинити" : "Пошук");
    GtkStyleContext *sc = gtk_widget_get_style_context(scan_btn);
    if (own_scan) gtk_style_context_add_class(sc, "active"); else gtk_style_context_remove_class(sc, "active");
    char buf[96];
    int n = n_connected();
    drawer_tab_icon(&blt, !on ? "\U000F00B2" : n ? "\U000F00B1" : "\U000F00AF", on ? "on" : "off");   /* 󰂲 вимкнено / 󰂱 підключено / 󰂯; зелений — увімкнено */
    const char *st = !adapter ? "Адаптера Bluetooth немає"
                   : pend_power == 1 ? "Вмикається…" : pend_power == 0 ? "Вимикається…"
                   : !on ? "Вимкнено"
                   : pend_scan == 1 ? "Запускаю пошук…" : pend_scan == 0 ? "Зупиняю пошук…"
                   : own_scan ? "Шукаю пристрої поруч…" : NULL;
    if (!st) {
        if (n) g_snprintf(buf, sizeof buf, "Увімкнено · підключено: %d", n);
        else g_strlcpy(buf, "Увімкнено", sizeof buf);
        st = buf;
    }
    gtk_label_set_text(GTK_LABEL(state_lbl), st);
    devconn_refresh();                             /* картки «підключити»: кнопка пошуку */
    GtkStyleContext *lc = gtk_widget_get_style_context(state_lbl);
    if (busy || own_scan) gtk_style_context_add_class(lc, "busy"); else gtk_style_context_remove_class(lc, "busy");
    gtk_label_set_text(GTK_LABEL(none), !adapter ? "" : !on ? "Увімкни Bluetooth, щоб бачити пристрої" : "Пристроїв немає — натисни «Пошук»");
}

/* Пошук: іде, доки не зупинять. Раз на TICK_S — оновити сигнал і порядок; якщо BlueZ
 * зупинив пошук сам (міст Switch 2, сон) — запустити знову */
static gboolean scan_tick(gpointer d) {
    if (!own_scan || !adapter) { scan_src = 0; return G_SOURCE_REMOVE; }
    if (pend_scan < 0 && !bln(adapter, "Discovering"))
        g_dbus_proxy_call(adapter, "StartDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
    rows_update_all();
    return G_SOURCE_CONTINUE;
}
static void scan_started(GObject *o, GAsyncResult *res, gpointer d) {
    GError *e = NULL;
    GVariant *v = g_dbus_proxy_call_finish(G_DBUS_PROXY(o), res, &e);
    if (v) g_variant_unref(v);
    pend_scan = -1;
    if (e && !strstr(e->message, "InProgress")) { say_err("Пошук", e); own_scan = FALSE; }
    else {
        own_scan = TRUE;
        if (!scan_src) scan_src = g_timeout_add_seconds(TICK_S, scan_tick, NULL);
    }
    g_clear_error(&e);
    head_update();
    rows_update_all();
}
static void scan_stopped(GObject *o, GAsyncResult *res, gpointer d) {
    GError *e = NULL;
    GVariant *v = g_dbus_proxy_call_finish(G_DBUS_PROXY(o), res, &e);
    if (v) g_variant_unref(v);
    g_clear_error(&e);                             /* «не шукали» — теж зупинено */
    pend_scan = -1;
    own_scan = FALSE;
    if (scan_src) { g_source_remove(scan_src); scan_src = 0; }
    head_update();
    rows_update_all();
}
static void on_scan(GtkButton *b, gpointer d) {
    if (!adapter || pend_scan >= 0) return;
    pend_scan = own_scan ? 0 : 1;
    pend_start();
    if (own_scan) g_dbus_proxy_call(adapter, "StopDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, scan_stopped, NULL);
    else {
        scan_t = g_get_monotonic_time();
        g_dbus_proxy_call(adapter, "StartDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, scan_started, NULL);
    }
    head_update();
}

static void rows_update_all(void) {
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, rows);
    while (g_hash_table_iter_next(&it, &k, &v)) row_update(v);
}

static void power_done(GObject *o, GAsyncResult *res, gpointer want) {
    GError *e = NULL;
    GVariant *v = g_dbus_proxy_call_finish(G_DBUS_PROXY(o), res, &e);
    if (v) g_variant_unref(v);
    if (e) {
        if (want && strstr(e->message, "Blocked")) {                     /* заблоковано rfkill — розблокувати й ще раз */
            char *argv[] = { "rfkill", "unblock", "bluetooth", NULL };
            if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL) && adapter)
                set_prop(adapter, ADAPTER, "Powered", g_variant_new_boolean(TRUE), power_done, NULL);
            else say_err("Увімкнути", e);
        } else { say_err(want ? "Увімкнути" : "Вимкнути", e); pend_power = -1; }
        g_error_free(e);
    }
    head_update();
}
static void power_done(GObject *o, GAsyncResult *res, gpointer d);
static gboolean power_on_later(gpointer d) {      /* після rfkill unblock: BlueZ зазвичай вмикає сам, інакше — Powered */
    if (adapter && !bln(adapter, "Powered")) set_prop(adapter, ADAPTER, "Powered", g_variant_new_boolean(TRUE), power_done, GINT_TO_POINTER(1));
    return G_SOURCE_REMOVE;
}
static gboolean on_power(GtkSwitch *s, gboolean st, gpointer d) {
    if (sw_updating || !adapter || pend_power >= 0) return TRUE;
    pend_power = st;                               /* перемикач неактивний, доки BlueZ не підтвердить */
    pend_start();
    if (!st && own_scan) {                         /* вимикаємо — пошук теж */
        own_scan = FALSE;
        if (scan_src) { g_source_remove(scan_src); scan_src = 0; }
    }
    head_update();
    if (!st) {                                     /* вимкнути = rfkill block: радіо повністю знеструмлене,
                                                      а systemd-rfkill пам'ятає це після перезавантаження/сну
                                                      (просте Powered=false BlueZ вмикає знову через AutoEnable) */
        char *argv[] = { "rfkill", "block", "bluetooth", NULL };
        if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, NULL, NULL, NULL, NULL)) return TRUE;   /* Powered стане false — head_update зніме очікування */
    }
    else {                                         /* увімкнути: спершу зняти rfkill (інакше BlueZ: «Blocked through rfkill») */
        char *argv[] = { "rfkill", "unblock", "bluetooth", NULL };
        g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, NULL, NULL, NULL, NULL);
        g_timeout_add(1200, power_on_later, NULL);
        return TRUE;
    }
    set_prop(adapter, ADAPTER, "Powered", g_variant_new_boolean(st), power_done, GINT_TO_POINTER(st));
    return TRUE;                                   /* стан перемкнеться, коли BlueZ підтвердить */
}

/* ───── події BlueZ */
static void find_adapter(void) {
    g_clear_object(&adapter);
    GList *objs = g_dbus_object_manager_get_objects(om);
    for (GList *l = objs; l && !adapter; l = l->next) {
        GDBusInterface *i = g_dbus_object_get_interface(l->data, ADAPTER);
        if (i) adapter = G_DBUS_PROXY(i);
    }
    g_list_free_full(objs, g_object_unref);
    head_update();
}

static void on_obj_added(GDBusObjectManager *m, GDBusObject *o, gpointer d) {
    if (!adapter) find_adapter();
    row_add(o);
}
static void on_obj_removed(GDBusObjectManager *m, GDBusObject *o, gpointer d) {
    const char *path = g_dbus_object_get_object_path(o);
    g_hash_table_remove(rows, path);
    if (adapter && !g_strcmp0(g_dbus_proxy_get_object_path(adapter), path)) find_adapter();
    resort();
}
static void on_iface_added(GDBusObjectManager *m, GDBusObject *o, GDBusInterface *i, gpointer d) { on_obj_added(m, o, d); }
static void on_iface_removed(GDBusObjectManager *m, GDBusObject *o, GDBusInterface *i, gpointer d) {
    const char *n = g_dbus_proxy_get_interface_name(G_DBUS_PROXY(i));
    if (!g_strcmp0(n, DEVICE) || !g_strcmp0(n, ADAPTER)) on_obj_removed(m, o, d);
}
static void on_props(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *i, GVariant *ch, GStrv inv, gpointer d) {
    const char *n = g_dbus_proxy_get_interface_name(i);
    if (!g_strcmp0(n, ADAPTER)) head_update();
    else if (!g_strcmp0(n, DEVICE)) {
        Row *r = row_by(g_dbus_object_get_object_path(G_DBUS_OBJECT(o)));
        if (r) row_update(r);
        head_update();                             /* «підключено: N» */
    }
}

void bt_init(void) {
    drawer_card_init(&blt, "bluetooth", "\U000F00AF", "Bluetooth", FALSE);   /* висувна панель біля «Сповіщень» */

    /* [значок] Bluetooth            [перемикач]
     *          стан словами
     *  список пристроїв
     *  [          Пошук          ]                                             */
    GtkWidget *head = ui_header(&blt, "\U000F00AF", "Bluetooth", NULL, &state_lbl);   /* 󰂯 */
    GList *hk = gtk_container_get_children(GTK_CONTAINER(head));
    add_class(hk->data, "bt-logo");                                    /* знак високий — трохи менший */
    g_list_free(hk);
    add_class(state_lbl, "bt-state");
    power_sw = gtk_switch_new();
    gtk_widget_set_can_focus(power_sw, FALSE);
    gtk_widget_set_valign(power_sw, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(power_sw, "Увімкнути / вимкнути Bluetooth");
    g_signal_connect(power_sw, "state-set", G_CALLBACK(on_power), NULL);
    gtk_box_pack_end(GTK_BOX(head), power_sw, FALSE, FALSE, 0);

    scan_btn = gtk_button_new_with_label("");
    add_class(scan_btn, "bt-scan");
    gtk_widget_set_can_focus(scan_btn, FALSE);
    gtk_widget_set_tooltip_text(scan_btn, "Шукати пристрої поруч, доки не зупиниш (список оновлюється раз на 5 с)");
    g_signal_connect(scan_btn, "clicked", G_CALLBACK(on_scan), NULL);

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll), TRUE);
    add_class(scroll, "ui-list");
    list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(scroll), list);
    none = gtk_label_new("");
    add_class(none, "bt-none");
    add_class(none, "ui-empty");
    gtk_label_set_xalign(GTK_LABEL(none), 0);
    status = gtk_label_new("");
    add_class(status, "bt-status");
    gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(status), 0);
    gtk_widget_set_no_show_all(status, TRUE);
    gtk_widget_set_no_show_all(none, TRUE);
    gtk_box_pack_start(GTK_BOX(blt.card), none, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(blt.card), scroll, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(blt.card), status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ui_actions(blt.card)), scan_btn, TRUE, TRUE, 0);
    gtk_widget_show_all(blt.card);

    rows = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, row_free);
    agent_init();
    om = g_dbus_object_manager_client_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                       BZ, "/", NULL, NULL, NULL, NULL, NULL);
    if (om) {
        g_signal_connect(om, "object-added", G_CALLBACK(on_obj_added), NULL);
        g_signal_connect(om, "object-removed", G_CALLBACK(on_obj_removed), NULL);
        g_signal_connect(om, "interface-added", G_CALLBACK(on_iface_added), NULL);
        g_signal_connect(om, "interface-removed", G_CALLBACK(on_iface_removed), NULL);
        g_signal_connect(om, "interface-proxy-properties-changed", G_CALLBACK(on_props), NULL);
        find_adapter();
        GList *objs = g_dbus_object_manager_get_objects(om);
        for (GList *l = objs; l; l = l->next) row_add(l->data);
        g_list_free_full(objs, g_object_unref);
    } else head_update();
    resort();
}

/* ───── для інших карток (devconn.c): ті самі дії, що й кліки в списку */
gboolean bt_device_act(const char *path) {
    Row *r = row_by(path);
    if (!r) return FALSE;
    if (r->busy) return TRUE;
    on_act(NULL, r);
    return TRUE;
}
void bt_scan_set(gboolean on) {
    if (!adapter || pend_scan >= 0 || own_scan == on) return;
    on_scan(NULL, NULL);
}
gboolean bt_scanning(void) { return own_scan || pend_scan == 1; }
gboolean bt_device_busy(const char *path) {
    Row *r = row_by(path);
    return (r && r->busy) || (hunt_phase && !g_strcmp0(path, hunt_path));
}
