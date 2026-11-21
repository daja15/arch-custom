/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  ПІДКЛЮЧИТИ — картки «Планшет» і «Контролери», коли нічого   │
 * │  такого не підключено (частина desk)                          │
 * │                                                              │
 * │  Список — пристрої з BlueZ потрібного типу (планшет: Icon    │
 * │  input-tablet / Wacom…; контролер: input-gaming / Controller…):│
 * │    • сполучені, але не підключені — «Підключити»;            │
 * │    • поруч і ще не сполучені (під час пошуку) — «Сполучити»;  │
 * │    • запам'ятовані, яких BlueZ уже не знає (забули / змінили │
 * │      адресу) — «Знайти»: пошук, і щойно пристрій з тією ж     │
 * │      назвою з'явиться в режимі сполучення — сполучається сам.│
 * │  Підключення робить Bluetooth-віджет (bt.c): він же шукає    │
 * │  пристрій, що змінив адресу, і сполучає заново.              │
 * │  Пам'ять пристроїв — ~/.config/hypr/bt-remember              │
 * │  («тип<TAB>назва»), × у рядку — забути.                       │
 * │  Підключився — картка ховається, з'являється жива картка     │
 * │  пристрою (tablet.c / pads.c). Сховано тими ж tablet.off /   │
 * │  pads.off.                                                   │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include "desk.h"
#include <json-glib/json-glib.h>

#define DEVICE "org.bluez.Device1"
#define DC_CARDS 0        /* окремі картки «підключити», коли нічого не підключено — вимкнено (користувач хотів інше:
                             сполучення з картки пристрою на кабелі, див. «USB → Bluetooth» нижче) */
#define WANT_S   120      /* скільки чекати режиму сполучення */

typedef struct {
    const char *kind, *title, *icon, *off;
    Card card;
    GtkWidget *list, *status, *scan_btn;
    gboolean online, built, want_pair;
    char *sig;                    /* що зараз показано (щоб не перебудовувати без змін) */
    guint status_src;
} Dc;
static Dc DC[2] = {
    { "tablet", "Планшет",    "\U000F0F49", "tablet.off", .card = { .pos_name = "desktabconn.pos", .off_name = "tablet.off" } },
    { "gaming", "Контролери", "\U000F0297", "pads.off",   .card = { .pos_name = "deskpadconn.pos", .off_name = "pads.off" } },
};
static GDBusObjectManager *om;
static guint upd_src;
static int last_dc = -1;
static void want_check(void);
/* USB → Bluetooth (див. нижче) */
typedef struct { GtkWidget *box, *lab, *btn; char *usb, *mac, *kind; gint64 msg_until; } Link;
static GPtrArray *links;
static char *want_usb, *want_kind;
static guint want_src;
static char *say_usb;                 /* чиї повідомлення bt.c показувати в картці */

static void links_refresh(void);          /* чия кнопка натиснута останньою — туди повідомлення bt.c */
static GPtrArray *mem;            /* "тип\tназва" */

/* ───── пам'ять */
static char *mem_path(void) { return g_build_filename(g_get_home_dir(), ".config/hypr/bt-remember", NULL); }
static void mem_load(void) {
    mem = g_ptr_array_new_with_free_func(g_free);
    char *f = mem_path(), *s = NULL;
    if (g_file_get_contents(f, &s, NULL, NULL)) {
        char **ls = g_strsplit(s, "\n", -1);
        for (char **l = ls; *l; l++) if (strchr(*l, '\t')) g_ptr_array_add(mem, g_strdup(*l));
        g_strfreev(ls);
    }
    g_free(s); g_free(f);
}
static void mem_save(void) {
    GString *s = g_string_new("# Запам'ятовані Bluetooth-пристрої для карток «Планшет» / «Контролери» (тип<TAB>назва)\n");
    for (guint i = 0; i < mem->len; i++) g_string_append_printf(s, "%s\n", (char *)mem->pdata[i]);
    char *f = mem_path();
    g_file_set_contents(f, s->str, -1, NULL);
    g_free(f);
    g_string_free(s, TRUE);
}
static gboolean mem_has(const char *kind, const char *name) {
    char *k = g_strdup_printf("%s\t%s", kind, name);
    gboolean r = FALSE;
    for (guint i = 0; i < mem->len && !r; i++) r = !strcmp(mem->pdata[i], k);
    g_free(k);
    return r;
}
static void mem_add(const char *kind, const char *name) {
    if (!name || mem_has(kind, name)) return;
    g_ptr_array_add(mem, g_strdup_printf("%s\t%s", kind, name));
    mem_save();
}
static void mem_del(const char *kind, const char *name) {
    char *k = g_strdup_printf("%s\t%s", kind, name);
    for (guint i = 0; i < mem->len; i++) if (!strcmp(mem->pdata[i], k)) { g_ptr_array_remove_index(mem, i); break; }
    g_free(k);
    mem_save();
}

/* ───── властивості */
static GVariant *prop(GDBusProxy *p, const char *k) { return g_dbus_proxy_get_cached_property(p, k); }
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
static gboolean heard(GDBusProxy *p) { GVariant *v = prop(p, "RSSI"); if (v) g_variant_unref(v); return v != NULL; }

/* 0 — планшет, 1 — контролер, −1 — інше */
static int kind_of(GDBusProxy *p) {
    char *icon = str(p, "Icon"), *name = str(p, "Name");
    int k = -1;
    if ((icon && strstr(icon, "tablet")) || (name && (strstr(name, "Wacom") || strstr(name, "Tablet") || strstr(name, "Huion") || strstr(name, "XP-Pen"))))
        k = 0;
    else if ((icon && strstr(icon, "gaming")) || (name && (strstr(name, "Controller") || strstr(name, "DualSense") ||
             strstr(name, "Joy-Con") || strstr(name, "Gamepad") || strstr(name, "Xbox"))))
        k = 1;
    g_free(icon); g_free(name);
    return k;
}

/* ───── повідомлення від bt.c (лише після нашої кнопки) */
static gboolean status_clear(gpointer d) { Dc *c = d; c->status_src = 0; gtk_widget_hide(c->status); return G_SOURCE_REMOVE; }
static void on_bt_say(const char *msg) {
    for (guint i = 0; say_usb && links && i < links->len; i++) {   /* повідомлення — у картку пристрою на кабелі */
        Link *k = links->pdata[i];
        if (strcmp(k->usb, say_usb)) continue;
        gtk_label_set_text(GTK_LABEL(k->lab), msg);
        k->msg_until = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;
    }
    if (last_dc < 0) return;
    Dc *c = &DC[last_dc];
    if (!c->status) return;
    gtk_label_set_text(GTK_LABEL(c->status), msg);
    gtk_widget_show(c->status);
    if (c->status_src) g_source_remove(c->status_src);
    c->status_src = g_timeout_add_seconds(10, status_clear, c);
}

/* ───── кнопки */
static void on_act(GtkButton *b, gpointer d) {
    Dc *c = d;
    last_dc = c - DC;
    const char *path = g_object_get_data(G_OBJECT(b), "path");
    if (path) { bt_device_act(path); return; }
    /* «Знайти»: пристрою немає в BlueZ — пошук; з'явиться з тією ж назвою — сполучимо */
    c->want_pair = TRUE;
    bt_scan_set(TRUE);
    on_bt_say("Шукаю… Переведи пристрій у режим сполучення (затисни кнопку, поки вогник не заблимає синім)");
}
static void on_forget(GtkButton *b, gpointer d) {
    Dc *c = d;
    mem_del(c->kind, g_object_get_data(G_OBJECT(b), "name"));
    g_clear_pointer(&c->sig, g_free);
}
static void on_scan(GtkButton *b, gpointer d) {
    Dc *c = d;
    last_dc = c - DC;
    gboolean on = !bt_scanning();
    c->want_pair = on;
    bt_scan_set(on);
}

static GtkWidget *row_new(Dc *c, const char *name, const char *sub, const char *btn, const char *path, gboolean forget) {
    GtkWidget *row = ui_row_box(ui_lead(c->icon), ui_text(name, sub, NULL, NULL));
    add_class(row, "ui-row");
    add_class(row, "dc-row");
    if (forget) {
        GtkWidget *x = gtk_button_new_with_label("\U000F0156");
        add_class(x, "bt-btn");
        gtk_widget_set_valign(x, GTK_ALIGN_CENTER);
        gtk_widget_set_can_focus(x, FALSE);
        gtk_widget_set_tooltip_text(x, "Забути (прибрати з цього списку)");
        g_object_set_data_full(G_OBJECT(x), "name", g_strdup(name), g_free);
        g_signal_connect(x, "clicked", G_CALLBACK(on_forget), c);
        gtk_box_pack_end(GTK_BOX(row), x, FALSE, FALSE, 0);
    }
    GtkWidget *b = gtk_button_new_with_label(btn);
    add_class(b, "ui-btn");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_can_focus(b, FALSE);
    if (path) g_object_set_data_full(G_OBJECT(b), "path", g_strdup(path), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(on_act), c);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    return row;
}

static gboolean enabled(Dc *c) {
    char *f = g_build_filename(g_get_home_dir(), ".config/hypr", c->off, NULL);
    gboolean on = !g_file_test(f, G_FILE_TEST_EXISTS);
    g_free(f);
    return on;
}

static void build_card(Dc *c) {
    if (c->built) return;
    c->built = TRUE;
    if (!load_pos(&c->card)) { c->card.mright = 420; c->card.mtop = c == &DC[0] ? 440 : 100; }
    card_init(&c->card, "desktop-devconn", "devconn");
    GtkWidget *sub = NULL;
    ui_header(&c->card, c->icon, c->title, NULL, &sub);
    gtk_label_set_text(GTK_LABEL(sub), "Не підключено");
    c->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(c->list, "ui-list");
    gtk_box_pack_start(GTK_BOX(c->card.card), c->list, FALSE, FALSE, 0);
    c->status = gtk_label_new("");
    add_class(c->status, "bt-status");
    gtk_label_set_line_wrap(GTK_LABEL(c->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(c->status), 0);
    gtk_widget_set_no_show_all(c->status, TRUE);
    gtk_box_pack_start(GTK_BOX(c->card.card), c->status, FALSE, FALSE, 0);
    c->scan_btn = gtk_button_new_with_label("");
    add_class(c->scan_btn, "bt-scan");
    gtk_widget_set_can_focus(c->scan_btn, FALSE);
    gtk_widget_set_tooltip_text(c->scan_btn, "Шукати пристрої поруч. Новий — переведи в режим сполучення");
    g_signal_connect(c->scan_btn, "clicked", G_CALLBACK(on_scan), c);
    gtk_box_pack_start(GTK_BOX(ui_actions(c->card.card)), c->scan_btn, TRUE, TRUE, 0);
}

static gboolean do_update(gpointer d) {
    upd_src = 0;
    if (!om) return G_SOURCE_REMOVE;
    GList *objs = g_dbus_object_manager_get_objects(om);
    for (int k = 0; k < 2; k++) {
        Dc *c = &DC[k];
        GString *sig = g_string_new(bt_scanning() ? "S" : "-");
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);   /* хто з пам'яті вже є в BlueZ */
        typedef struct { char *name, *sub, *btn, *path; gboolean forget; } R;
        GArray *rows = g_array_new(FALSE, TRUE, sizeof(R));
        for (GList *l = objs; l; l = l->next) {
            GDBusInterface *i = g_dbus_object_get_interface(l->data, DEVICE);
            if (!i) continue;
            GDBusProxy *p = G_DBUS_PROXY(i);
            if (kind_of(p) == k) {
                const char *path = g_dbus_object_get_object_path(l->data);
                gboolean paired = bln(p, "Paired") || bln(p, "Bonded"), conn = bln(p, "Connected"), h = heard(p);
                char *name = str(p, "Name"), *alias = str(p, "Alias");
                if (paired && name) mem_add(c->kind, name);
                if (name && paired) g_ptr_array_add(names, g_strdup(name));
                gboolean busy = bt_device_busy(path);
                if (!conn && (paired || (h && name))) {
                    if (!paired && c->want_pair && name && mem_has(c->kind, name) && !busy) {   /* наш, у режимі сполучення */
                        last_dc = k;
                        c->want_pair = FALSE;
                        bt_device_act(path);
                        busy = TRUE;
                    }
                    R r = { g_strdup(alias ? alias : name), g_strdup(busy ? "зачекай…" : paired ? (h ? "поруч · сполучено" : "сполучено")
                                                                       : "поруч · ще не сполучено"),
                            g_strdup(busy ? "…" : paired ? "Підключити" : "Сполучити"), g_strdup(path), FALSE };
                    g_array_append_val(rows, r);
                    g_string_append_printf(sig, "|%s%s%d%d%d", path, r.name, paired, h, busy);
                }
                g_free(name); g_free(alias);
            }
            g_object_unref(i);
        }
        for (guint m = 0; m < mem->len; m++) {                       /* запам'ятовані, яких BlueZ не знає */
            char *e = mem->pdata[m], *tab = strchr(e, '\t');
            if (strncmp(e, c->kind, tab - e) || strlen(c->kind) != (size_t)(tab - e)) continue;
            gboolean known = FALSE;
            for (guint n = 0; n < names->len; n++) known |= !strcmp(names->pdata[n], tab + 1);
            if (known) continue;
            R r = { g_strdup(tab + 1), g_strdup(c->want_pair && bt_scanning() ? "шукаю… режим сполучення?" : "не сполучено"),
                    g_strdup("Знайти"), NULL, TRUE };
            g_array_append_val(rows, r);
            g_string_append_printf(sig, "|m%s%d", tab + 1, c->want_pair);
        }
        gboolean show = DC_CARDS && enabled(c) && !c->online && rows->len > 0;
        if (show) {
            build_card(c);
            if (g_strcmp0(sig->str, c->sig)) {
                g_free(c->sig); c->sig = g_strdup(sig->str);
                GList *kids = gtk_container_get_children(GTK_CONTAINER(c->list));
                for (GList *l = kids; l; l = l->next) gtk_widget_destroy(l->data);
                g_list_free(kids);
                for (guint r = 0; r < rows->len; r++) {
                    R *x = &g_array_index(rows, R, r);
                    gtk_box_pack_start(GTK_BOX(c->list), row_new(c, x->name, x->sub, x->btn, x->path, x->forget), FALSE, FALSE, 0);
                }
                gtk_button_set_label(GTK_BUTTON(c->scan_btn), bt_scanning() ? "Зупинити пошук" : "Знайти поблизу");
                gtk_widget_show_all(c->list);
                gtk_window_resize(GTK_WINDOW(c->card.win), 1, 1);
            }
            if (!gtk_widget_get_visible(c->card.win)) { gtk_widget_show_all(c->card.win); cards_relayout(); }
        } else if (c->built && gtk_widget_get_visible(c->card.win)) { gtk_widget_hide(c->card.win); cards_relayout(); }
        for (guint r = 0; r < rows->len; r++) { R *x = &g_array_index(rows, R, r); g_free(x->name); g_free(x->sub); g_free(x->btn); g_free(x->path); }
        g_array_free(rows, TRUE);
        g_ptr_array_unref(names);
        g_string_free(sig, TRUE);
    }
    g_list_free_full(objs, g_object_unref);
    want_check();
    links_refresh();
    return G_SOURCE_REMOVE;
}
static void update(void) { if (!upd_src) upd_src = g_timeout_add(400, do_update, NULL); }

/* ═════════════ USB → BLUETOOTH ═════════════
 * Пристрій на кабелі (планшет, геймпад): у його картці — «Сполучити по Bluetooth».
 * Клік — пошук; щойно в ефірі з'явиться той самий пристрій (та сама MAC-адреса, якщо драйвер
 * її знає, або назва Bluetooth — частина назви на кабелі) у режимі сполучення — сполучаємо,
 * довіряємо й запам'ятовуємо. Витягнули кабель — підключаємо по Bluetooth самі
 * (змінив адресу — bt.c знайде за назвою). */

static gboolean name_match(const char *usb, const char *bt) { return usb && bt && strlen(bt) >= 4 && strstr(usb, bt); }
/* Пристрій BlueZ для цього кабельного (paired_only — лише сполучений). Повертає шлях (g_free) */
static char *find_bt(const char *usb, const char *mac, gboolean paired_only, gboolean heard_only, gboolean *connected) {
    if (!om) return NULL;
    char *res = NULL;
    int best = 0;
    GList *objs = g_dbus_object_manager_get_objects(om);
    for (GList *l = objs; l; l = l->next) {
        GDBusInterface *i = g_dbus_object_get_interface(l->data, DEVICE);
        if (!i) continue;
        GDBusProxy *p = G_DBUS_PROXY(i);
        gboolean paired = bln(p, "Paired") || bln(p, "Bonded");
        char *addr = str(p, "Address"), *name = str(p, "Name");
        int score = mac && addr && !g_ascii_strcasecmp(mac, addr) ? 2 : name_match(usb, name) ? 1 : 0;
        if (score > best && (!paired_only || paired) && (!heard_only || heard(p))) {
            best = score;
            g_free(res);
            res = g_strdup(g_dbus_object_get_object_path(l->data));
            if (connected) *connected = bln(p, "Connected");
        }
        g_free(addr); g_free(name);
        g_object_unref(i);
    }
    g_list_free_full(objs, g_object_unref);
    return res;
}
static const char *pair_hint(const char *n) {
    if (n && strstr(n, "Wacom")) return "Витягни кабель і затисни кнопку живлення, поки вогник не заблимає синім";
    if (n && (strstr(n, "DualSense") || strstr(n, "Wireless Controller")))
        return "Витягни кабель і затисни Create/Share + PS, поки світло не заблимає";
    if (n && (strstr(n, "Pro Controller") || strstr(n, "Joy-Con"))) return "Витягни кабель і натисни маленьку кнопку синхронізації";
    return "Витягни кабель і ввімкни на пристрої режим сполучення";
}
/* Switch 2 (Pro Controller 2, Joy-Con 2, GameCube): по Bluetooth їх веде не BlueZ, а міст switch2-bt
 * (~/.local/share/switch2-bt, ~/.config/nso-gc/config.json). Звичайне сполучення BlueZ тут не працює,
 * а наш пошук ефіру ще й заважає мосту підключитись — тому для них лише стан з config.json. */
static gboolean is_switch2(const char *usb) { return usb && (strstr(usb, "Switch 2") || strstr(usb, "Joy-Con 2") || strstr(usb, "NSO")); }
static gboolean bridge_bonded(const char *usb) {
    const char *want = strstr(usb, "Pro Controller") ? "Pro Controller" : strstr(usb, "Joy-Con") ? "Joy-Con" : strstr(usb, "GameCube") ? "GameCube" : "";
    char *f = g_build_filename(g_get_user_config_dir(), "nso-gc", "config.json", NULL);
    gboolean ok = FALSE;
    JsonParser *jp = json_parser_new();
    if (json_parser_load_from_file(jp, f, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) {
        JsonObject *o = json_node_get_object(json_parser_get_root(jp));
        JsonArray *a = json_object_has_member(o, "controllers") ? json_object_get_array_member(o, "controllers") : NULL;
        for (guint i = 0; a && i < json_array_get_length(a); i++) {
            JsonObject *c = json_array_get_object_element(a, i);
            if (strstr(json_object_get_string_member_with_default(c, "name", ""), want) &&
                json_object_get_boolean_member_with_default(c, "bonded", FALSE)) ok = TRUE;
        }
    }
    g_object_unref(jp); g_free(f);
    return ok;
}
static void on_bridge_pair(GtkButton *b, gpointer d) {   /* сполучення мостом: окреме вікно-термінал */
    char *argv[] = { "kitty", "--class", "floating-update", "-e", "sh", "-c",
        "echo 'Сполучення через міст Switch 2. Затисни кнопку синхронізації на контролері, поки вогники не побіжать.'; "
        "systemctl --user stop switch2-bt; cd ~/.local/share/switch2-bt && .venv/bin/python -m ngc pair; "
        "systemctl --user start switch2-bt; echo; echo 'Готово — Enter, щоб закрити'; read x", NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void link_refresh(Link *k) {
    if (g_get_monotonic_time() < k->msg_until) return;   /* поки видно повідомлення bt.c */
    if (is_switch2(k->usb)) {
        gboolean ok = bridge_bonded(k->usb);
        gtk_label_set_text(GTK_LABEL(k->lab), ok ? "Сполучено через міст Switch 2. Витягнеш кабель — натисни HOME, і підключиться"
                                                 : "Через міст Switch 2 ще не сполучено");
        gtk_button_set_label(GTK_BUTTON(k->btn), "Сполучити через міст");
        gtk_widget_set_visible(k->btn, !ok);
        return;
    }
    char *path = find_bt(k->usb, k->mac, TRUE, FALSE, NULL);
    gboolean wanting = want_usb && !strcmp(want_usb, k->usb);
    if (path) {
        gtk_label_set_text(GTK_LABEL(k->lab), "Сполучено по Bluetooth. Витягнеш кабель — підключиться сам");
        gtk_widget_hide(k->btn);
    } else {
        char *s = wanting ? g_strdup_printf("Чекаю режиму сполучення: %s", pair_hint(k->usb))
                          : g_strdup("По Bluetooth ще не сполучено");
        gtk_label_set_text(GTK_LABEL(k->lab), s);
        g_free(s);
        gtk_button_set_label(GTK_BUTTON(k->btn), wanting ? "Скасувати" : "\U000F00AF  Сполучити по Bluetooth");
        gtk_widget_show(k->btn);
    }
    g_free(path);
}
static void links_refresh(void) { for (guint i = 0; links && i < links->len; i++) link_refresh(links->pdata[i]); }

static void want_stop(void) {
    if (want_src) { g_source_remove(want_src); want_src = 0; }
    g_clear_pointer(&want_usb, g_free); g_clear_pointer(&want_kind, g_free);
    bt_scan_set(FALSE);
    links_refresh();
}
static gboolean want_timeout(gpointer d) {
    want_src = 0;
    char *u = g_strdup(want_usb);
    want_stop();
    for (guint i = 0; links && i < links->len; i++) {
        Link *k = links->pdata[i];
        if (!g_strcmp0(k->usb, u)) {
            gtk_label_set_text(GTK_LABEL(k->lab), "Не дочекався режиму сполучення — спробуй ще раз");
            k->msg_until = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;
        }
    }
    g_free(u);
    return G_SOURCE_REMOVE;
}
static void on_link_btn(GtkButton *b, Link *k) {
    if (is_switch2(k->usb)) { on_bridge_pair(b, NULL); return; }
    if (want_usb && !strcmp(want_usb, k->usb)) { want_stop(); return; }
    want_stop();
    want_usb = g_strdup(k->usb); want_kind = g_strdup(k->kind);
    g_free(say_usb); say_usb = g_strdup(k->usb);
    last_dc = -1;
    want_src = g_timeout_add_seconds(WANT_S, want_timeout, NULL);
    bt_scan_set(TRUE);
    links_refresh();
    update();
}
/* Пошук іде — чи не з'явився наш у режимі сполучення? */
static void want_check(void) {
    if (!want_usb) return;
    char *path = find_bt(want_usb, NULL, FALSE, TRUE, NULL);
    if (!path) return;
    GDBusObject *o = g_dbus_object_manager_get_object(om, path);
    GDBusInterface *i = o ? g_dbus_object_get_interface(o, DEVICE) : NULL;
    gboolean paired = i && (bln(G_DBUS_PROXY(i), "Paired") || bln(G_DBUS_PROXY(i), "Bonded"));
    char *name = i ? str(G_DBUS_PROXY(i), "Name") : NULL;
    if (!paired && !bt_device_busy(path)) {
        mem_add(want_kind, name);
        g_free(say_usb); say_usb = g_strdup(want_usb);
        if (want_src) { g_source_remove(want_src); want_src = 0; }
        g_clear_pointer(&want_usb, g_free); g_clear_pointer(&want_kind, g_free);
        bt_scan_set(FALSE);
        bt_device_act(path);                                 /* сполучити → довіряти → підключити (bt.c) */
    }
    g_free(name);
    if (i) g_object_unref(i);
    if (o) g_object_unref(o);
    g_free(path);
}

static void link_gone(GtkWidget *w, Link *k) {
    g_ptr_array_remove(links, k);
    g_free(k->usb); g_free(k->mac); g_free(k->kind); g_free(k);
}
GtkWidget *btlink_new(const char *usb_name, const char *mac, const char *kind) {
    if (!links) links = g_ptr_array_new();
    Link *k = g_new0(Link, 1);
    k->usb = g_strdup(usb_name); k->mac = g_strdup(mac); k->kind = g_strdup(kind);
    k->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    add_class(k->box, "bl-box");
    ui_section(k->box, "Bluetooth");
    k->lab = gtk_label_new("");
    add_class(k->lab, "t-caption");
    gtk_label_set_line_wrap(GTK_LABEL(k->lab), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(k->lab), 1);
    gtk_label_set_xalign(GTK_LABEL(k->lab), 0);
    k->btn = gtk_button_new_with_label("");
    add_class(k->btn, "ui-btn");
    gtk_widget_set_can_focus(k->btn, FALSE);
    gtk_widget_set_no_show_all(k->btn, TRUE);
    g_signal_connect(k->btn, "clicked", G_CALLBACK(on_link_btn), k);
    gtk_box_pack_start(GTK_BOX(k->box), k->lab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(k->box), k->btn, FALSE, FALSE, 0);
    g_signal_connect(k->box, "destroy", G_CALLBACK(link_gone), k);
    g_ptr_array_add(links, k);
    link_refresh(k);
    return k->box;
}

/* Кабель витягнули — підключити по Bluetooth (якщо сполучено) */
typedef struct { char *usb, *mac; int tries; } Unplug;
static gboolean unplug_go(gpointer d) {
    Unplug *u = d;
    gboolean conn = FALSE;
    char *path = find_bt(u->usb, u->mac, TRUE, FALSE, &conn);
    if (path && !conn && !bt_device_busy(path)) { g_free(say_usb); say_usb = NULL; bt_device_act(path); }
    gboolean again = !path && ++u->tries < 3;                 /* BlueZ ще міг не оновитись */
    g_free(path);
    if (again) return G_SOURCE_CONTINUE;
    g_free(u->usb); g_free(u->mac); g_free(u);
    return G_SOURCE_REMOVE;
}
void btlink_unplugged(const char *usb_name, const char *mac) {
    if (!usb_name || is_switch2(usb_name)) return;          /* Switch 2 — підключає міст сам */
    Unplug *u = g_new0(Unplug, 1);
    u->usb = g_strdup(usb_name); u->mac = g_strdup(mac);
    g_timeout_add(1500, unplug_go, u);                         /* дати пристрою перемкнутись на Bluetooth */
}

void devconn_online(const char *kind, gboolean on) {
    for (int k = 0; k < 2; k++) if (!strcmp(DC[k].kind, kind) && DC[k].online != on) { DC[k].online = on; DC[k].want_pair = FALSE; }
    update();
}
void devconn_refresh(void) { for (int k = 0; k < 2; k++) g_clear_pointer(&DC[k].sig, g_free); update(); }

static void on_change(void) { update(); }
static void on_obj(GDBusObjectManager *m, GDBusObject *o, gpointer d) { on_change(); }
static void on_iface(GDBusObjectManager *m, GDBusObject *o, GDBusInterface *i, gpointer d) { on_change(); }
static void on_props(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *i, GVariant *ch, GStrv inv, gpointer d) { on_change(); }

void devconn_init(void) {
    mem_load();
    bt_say_hook = on_bt_say;
    om = g_dbus_object_manager_client_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                       "org.bluez", "/", NULL, NULL, NULL, NULL, NULL);
    if (!om) return;
    g_signal_connect(om, "object-added", G_CALLBACK(on_obj), NULL);
    g_signal_connect(om, "object-removed", G_CALLBACK(on_obj), NULL);
    g_signal_connect(om, "interface-added", G_CALLBACK(on_iface), NULL);
    g_signal_connect(om, "interface-removed", G_CALLBACK(on_iface), NULL);
    g_signal_connect(om, "interface-proxy-properties-changed", G_CALLBACK(on_props), NULL);
    g_timeout_add_seconds(2, (GSourceFunc)do_update, NULL);         /* і раз через 2 с (після планшетів/геймпадів) */
    update();
}
