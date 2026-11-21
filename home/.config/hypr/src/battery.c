/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  ЗАРЯД — віджет робочого столу (частина desk)                │
 * │  Усі бездротові пристрої з батареєю, крім геймпадів (у них   │
 * │  свої картки): навушники, мишка, клавіатура, телефон…        │
 * │  Рядок: значок за типом, назва, смужка й відсотки;           │
 * │  заряджається — блискавка. Спершу — ті, де заряду найменше.  │
 * │  Батарея ноутбука й живлення від мережі — не тут.            │
 * │  Джерело — UPower (системна шина D-Bus): заряд з Bluetooth;   │
 * │  мишки / клавіатури Logitech на радіоприймачі — напряму       │
 * │  (HID++, як Solaar), див. розділ LOGITECH нижче.             │
 * │  Без опитування: лише події UPower (з'явився / зник /        │
 * │  змінився заряд). Немає пристроїв — картки не видно.         │
 * │  Сховати / показати — desk.sh toggle battery                 │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include "desk.h"

Card bat = { .pos_name = "deskbattery.pos", .off_name = "battery.off" };

#define UP_NAME  "org.freedesktop.UPower"
#define UP_PATH  "/org/freedesktop/UPower"
#define UP_DEV   "org.freedesktop.UPower.Device"
#define LOW      20            /* % і менше — червоним */

/* Типи пристроїв UPower (поле Type) */
enum { UT_LINE = 1, UT_BATTERY = 2, UT_UPS = 3, UT_MOUSE = 5, UT_KEYBOARD = 6, UT_PHONE = 8, UT_TABLET = 10,
       UT_COMPUTER = 11, UT_GAMING = 12, UT_PEN = 13, UT_TOUCHPAD = 14, UT_HEADSET = 17, UT_SPEAKERS = 18,
       UT_HEADPHONES = 19, UT_REMOTE = 22, UT_PRINTER = 23, UT_CAMERA = 25, UT_WEARABLE = 26 };

typedef struct {
    char *path;
    GDBusProxy *px;
    GtkWidget *row, *icon, *name, *pct, *bar;
    double level;              /* для сортування; −1 — не показано */
} Dev;

static GDBusConnection *sys;
static GHashTable *devs;       /* шлях → Dev* */
static GtkWidget *list;
static guint sort_src;

/* ───── властивості з кешу проксі */
static guint u32(GDBusProxy *p, const char *k) {
    GVariant *v = g_dbus_proxy_get_cached_property(p, k);
    guint r = v && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(v) : 0;
    if (v) g_variant_unref(v);
    return r;
}
static double dbl(GDBusProxy *p, const char *k) {
    GVariant *v = g_dbus_proxy_get_cached_property(p, k);
    double r = v && g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE) ? g_variant_get_double(v) : 0;
    if (v) g_variant_unref(v);
    return r;
}
static gboolean bln(GDBusProxy *p, const char *k) {
    GVariant *v = g_dbus_proxy_get_cached_property(p, k);
    gboolean r = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) ? g_variant_get_boolean(v) : FALSE;
    if (v) g_variant_unref(v);
    return r;
}
static char *str(GDBusProxy *p, const char *k) {
    GVariant *v = g_dbus_proxy_get_cached_property(p, k);
    char *r = v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_dup_string(v, NULL) : g_strdup("");
    if (v) g_variant_unref(v);
    return r;
}

static const char *type_icon(guint t) {
    switch (t) {
    case UT_MOUSE:      return "\U000F037D";   /* 󰍽 */
    case UT_TOUCHPAD:   return "\U000F07F8";
    case UT_KEYBOARD:   return "\U000F030C";
    case UT_PHONE:      return "\U000F011C";
    case UT_TABLET:     return "\U000F04F6";
    case UT_PEN:        return "\U000F03EA";
    case UT_HEADSET:    return "\U000F02CE";
    case UT_HEADPHONES: return "\U000F02CB";
    case UT_SPEAKERS:   return "\U000F04C3";
    case UT_REMOTE:     return "\U000F0454";
    case UT_PRINTER:    return "\U000F042A";
    case UT_CAMERA:     return "\U000F0100";
    case UT_WEARABLE:   return "\U000F0589";
    default:            return "\U000F00AF";   /* Bluetooth — інше */
    }
}

/* Геймпад? (тип «gaming-input», або назва — на випадок, коли UPower не знає типу) */
static gboolean is_pad(guint type, const char *model) {
    if (type == UT_GAMING) return TRUE;
    if (type == UT_TABLET || type == UT_PEN) return TRUE;   /* планшет — свій заряд у картці «Планшет» (tablet.c) */
    if (strstr(model, "Wacom") || strstr(model, "Intuos")) return TRUE;
    static const char *W[] = { "Controller", "Joy-Con", "DualSense", "DualShock", "Gamepad", "Xbox", NULL };
    for (const char **w = W; *w; w++) if (strstr(model, *w)) return TRUE;
    return FALSE;
}

/* Відсоток; деякі пристрої дають лише грубий рівень (BatteryLevel) — переводимо приблизно */
static double percent(GDBusProxy *p) {
    guint lvl = u32(p, "BatteryLevel");
    double pc = dbl(p, "Percentage");
    if (lvl > 1 && pc <= 0) {
        switch (lvl) { case 3: return 10; case 4: return 5; case 5: return 50; case 6: return 50; case 7: return 80; case 8: return 100; }
    }
    return pc;
}

/* ───── порядок і видимість картки */
static gint by_level(gconstpointer a, gconstpointer b) {
    const Dev *x = *(Dev *const *)a, *y = *(Dev *const *)b;
    if (x->level != y->level) return x->level < y->level ? -1 : 1;
    return g_strcmp0(x->path, y->path);
}
static gboolean do_sort(gpointer d) {
    sort_src = 0;
    GPtrArray *a = g_ptr_array_new();
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, devs);
    while (g_hash_table_iter_next(&it, &k, &v)) if (((Dev *)v)->level >= 0) g_ptr_array_add(a, v);
    g_ptr_array_sort(a, by_level);
    for (guint i = 0; i < a->len; i++) gtk_box_reorder_child(GTK_BOX(list), ((Dev *)a->pdata[i])->row, i);
    gboolean empty = a->len == 0;
    g_ptr_array_unref(a);
    if (empty != bat.empty) { bat.empty = empty; cards_apply(NULL); }
    gtk_window_resize(GTK_WINDOW(bat.win), 1, 1);            /* картка під вміст */
    return G_SOURCE_REMOVE;
}
static void resort(void) { if (!sort_src) sort_src = g_idle_add(do_sort, NULL); }

/* ───── рядок: показати (level < 0 — сховати). tip — підказка (NULL — назва) */
static void dev_show(Dev *d, double level, guint type, const char *name, gboolean charging, const char *pct_text, const char *tip) {
    gboolean show = level >= 0;
    gtk_widget_set_visible(d->row, show);
    d->level = level;
    if (show) {
        gtk_label_set_text(GTK_LABEL(d->icon), type_icon(type));
        gtk_label_set_text(GTK_LABEL(d->name), name);
        gtk_widget_set_tooltip_text(d->row, tip ? tip : name);
        char buf[48];
        g_snprintf(buf, sizeof buf, "%s%.0f%%", charging ? "\U000F0084 " : "", d->level);   /* 󰂄 */
        gtk_label_set_text(GTK_LABEL(d->pct), pct_text ? pct_text : buf);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(d->bar), CLAMP(d->level, 0, 100));
        gtk_widget_set_visible(d->bar, pct_text == NULL);     /* підказка («?») — без смужки */
        GtkStyleContext *sc = gtk_widget_get_style_context(d->row);
        if (d->level <= LOW && !charging) gtk_style_context_add_class(sc, "low");
        else gtk_style_context_remove_class(sc, "low");
        if (charging) gtk_style_context_add_class(sc, "charging");
        else gtk_style_context_remove_class(sc, "charging");
    }
    resort();
}

static void dev_update(Dev *d) {                   /* пристрій UPower */
    guint type = u32(d->px, "Type");
    char *model = str(d->px, "Model"), *vendor = str(d->px, "Vendor");
    gboolean show = bln(d->px, "IsPresent") && !bln(d->px, "PowerSupply") &&
                    type != UT_LINE && type != UT_BATTERY && type != UT_UPS && type != UT_COMPUTER &&
                    !is_pad(type, model) && !g_str_has_suffix(d->path, "/DisplayDevice");
    guint state = u32(d->px, "State");
    dev_show(d, show ? percent(d->px) : -1, type, *model ? model : *vendor ? vendor : "Пристрій",
             state == 1 || state == 5, NULL, NULL);      /* заряджається / чекає заряду */
    g_free(model); g_free(vendor);
}

static void on_props(GDBusProxy *p, GVariant *changed, GStrv inval, Dev *d) { dev_update(d); }

static void dev_free(gpointer x) {
    Dev *d = x;
    if (d->row) gtk_widget_destroy(d->row);
    if (d->px) g_object_unref(d->px);
    g_free(d->path); g_free(d);
}

static Dev *row_new(const char *path) {
    Dev *d = g_new0(Dev, 1);
    d->path = g_strdup(path);
    d->level = -1;
    d->row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);   /* [значок] 12 [назва ··· %] / смужка */
    add_class(d->row, "b-row");
    d->icon = ui_lead("");
    add_class(d->icon, "b-icon");
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    d->name = gtk_label_new("");
    add_class(d->name, "b-name");
    gtk_label_set_ellipsize(GTK_LABEL(d->name), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(d->name), 0);
    d->pct = gtk_label_new("");
    add_class(d->pct, "b-pct");
    d->bar = gtk_level_bar_new_for_interval(0, 100);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(d->bar), GTK_LEVEL_BAR_OFFSET_LOW);    /* колір — наш, з CSS */
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(d->bar), GTK_LEVEL_BAR_OFFSET_HIGH);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(d->bar), GTK_LEVEL_BAR_OFFSET_FULL);
    add_class(d->bar, "b-bar");
    gtk_widget_set_no_show_all(d->row, TRUE);      /* видимість рядка — лише наша (show_all картки її не чіпає) */
    gtk_box_pack_start(GTK_BOX(top), d->name, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), d->pct, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mid), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mid), d->bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(d->row), d->icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(d->row), mid, TRUE, TRUE, 0);
    gtk_widget_show_all(mid);
    gtk_widget_show(d->icon);
    gtk_box_pack_start(GTK_BOX(list), d->row, FALSE, FALSE, 0);
    g_hash_table_insert(devs, d->path, d);
    return d;
}

static void dev_add(const char *path) {
    if (g_hash_table_contains(devs, path)) return;
    GDBusProxy *px = g_dbus_proxy_new_sync(sys, G_DBUS_PROXY_FLAGS_NONE, NULL, UP_NAME, path, UP_DEV, NULL, NULL);
    if (!px) return;
    Dev *d = row_new(path);
    d->px = px;
    g_signal_connect(px, "g-properties-changed", G_CALLBACK(on_props), d);
    dev_update(d);
}

static void on_added(GDBusConnection *c, const char *s, const char *o, const char *i, const char *sig, GVariant *p, gpointer u) {
    const char *path;
    g_variant_get(p, "(&o)", &path);
    dev_add(path);
}
static void on_removed(GDBusConnection *c, const char *s, const char *o, const char *i, const char *sig, GVariant *p, gpointer u) {
    const char *path;
    g_variant_get(p, "(&o)", &path);
    g_hash_table_remove(devs, path);
    resort();
}

static void enumerate(void) {
    GVariant *r = g_dbus_connection_call_sync(sys, UP_NAME, UP_PATH, UP_NAME, "EnumerateDevices", NULL,
                                              G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
    if (!r) return;
    GVariantIter *it;
    const char *path;
    g_variant_get(r, "(ao)", &it);
    while (g_variant_iter_next(it, "&o", &path)) dev_add(path);
    g_variant_iter_free(it);
    g_variant_unref(r);
}

/* UPower перезапустився — перечитати все */
static gboolean is_up(gpointer k, gpointer v, gpointer u) { return ((Dev *)v)->px != NULL; }
static void on_owner(GDBusConnection *c, const char *name, const char *owner, gpointer u) {
    g_hash_table_foreach_remove(devs, is_up, NULL);
    enumerate();
    resort();
}
static void on_lost(GDBusConnection *c, const char *name, gpointer u) { g_hash_table_foreach_remove(devs, is_up, NULL); resort(); }


/* ═════════════ LOGITECH (HID++) ═════════════
 * Приймачі Logitech (Bolt, Unifying…) ядро не завжди розпізнає (тоді в UPower їх немає).
 * Питаємо самі, як Solaar: через /dev/hidrawN того інтерфейсу приймача, що говорить HID++
 * (у описі звітів є звіти 0x10 / 0x11). Для кожного з 6 місць приймача: чи є пристрій,
 * назва й тип (функція 0x0005), заряд (0x1004, інакше 0x1000, інакше напруга 0x1001).
 * Доступ до hidraw — лише root, доки нема правила udev (його ставить пакет solaar).
 * Опитування — раз на LG_EVERY_S у окремому потоці (кілька коротких запитів, ~0 навантаження).
 * Пристрій спить (мишку не рухали) — не відповідає; тоді показуємо останнє відоме значення. */
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>

#define LG_EVERY_S 120
#define LG_SWID    0x0B         /* наш «номер програми» у запитах — щоб не плутати з відповідями Solaar */
#define LG_KEEP_S  3600         /* сплячий пристрій — показувати останній заряд стільки */

typedef struct { char *key, *name; int type, pct; gboolean charging, unknown; gint64 seen; } LgRes;   /* unknown — приймач без даних про заряд */
static GHashTable *lg_last;     /* key → LgRes* (останнє відоме) */
static gboolean lg_denied;      /* є приймач, але немає доступу */
static gboolean lg_busy;

/* Запит: коротке повідомлення 0x10 (до 3 байт параметрів). Відповідь — 0x10 / 0x11 з тим самим
 * пристроєм, функцією й swid; помилка — 0x8F (HID++ 1.0) або 0xFF (2.0). Повертає довжину параметрів або −1. */
static int hpp_t(int fd, int dev, int feat, int func, const guint8 *par, int np, guint8 *out, int wait_ms) {
    guint8 req[7] = { 0x10, dev, feat, (func << 4) | LG_SWID, 0, 0, 0 };
    for (int i = 0; i < np && i < 3; i++) req[4 + i] = par[i];
    while (TRUE) {                                 /* викинути старе з черги */
        struct pollfd pf = { fd, POLLIN, 0 };
        guint8 junk[64];
        if (poll(&pf, 1, 0) <= 0 || read(fd, junk, sizeof junk) <= 0) break;
    }
    if (write(fd, req, sizeof req) != sizeof req) return -1;
    gint64 end = g_get_monotonic_time() + (gint64)wait_ms * 1000;
    while (g_get_monotonic_time() < end) {
        struct pollfd pf = { fd, POLLIN, 0 };
        int ms = (int)((end - g_get_monotonic_time()) / 1000);
        if (poll(&pf, 1, MAX(ms, 1)) <= 0) return -1;
        guint8 r[64];
        int n = read(fd, r, sizeof r);
        if (n < 7 || (r[0] != 0x10 && r[0] != 0x11) || r[1] != dev) continue;
        if (r[2] == 0x8F || r[2] == 0xFF) {        /* помилка на наш запит? */
            if (r[3] == feat && r[4] == ((func << 4) | LG_SWID)) return -1;
            continue;
        }
        if (r[2] != feat || r[3] != ((func << 4) | LG_SWID)) continue;
        memcpy(out, r + 4, n - 4);
        return n - 4;
    }
    return -1;
}
static int hpp(int fd, int dev, int feat, int func, const guint8 *par, int np, guint8 *out) {
    return hpp_t(fd, dev, feat, func, par, np, out, 800);
}
static int feature(int fd, int dev, guint16 id) {   /* номер функції (0 — немає) */
    guint8 p[2] = { id >> 8, id & 0xFF }, o[64];
    return hpp(fd, dev, 0, 0, p, 2, o) > 0 ? o[0] : 0;
}

/* тип HID++ (0x0005) → тип UPower для значка; −1 — геймпад/кермо (не показуємо) */
static int lg_type(int t) {
    switch (t) {
    case 0: case 2: return UT_KEYBOARD;
    case 1: case 6: return UT_REMOTE;
    case 3: case 5: return UT_MOUSE;
    case 4: return UT_TOUCHPAD;
    case 8: return UT_HEADSET;
    case 9: return UT_CAMERA;
    case 10: case 11: case 12: return -1;
    default: return 0;
    }
}

static LgRes *lg_query(int fd, int dev, const char *hid) {
    guint8 o[64], ping[3] = { 0, 0, 0x5A };
    if (hpp_t(fd, dev, 0, 1, ping, 3, o, 2500) < 3) return NULL;   /* немає / спить (сплячий прокидається до ~1 с) */
    LgRes *r = g_new0(LgRes, 1);
    r->key = g_strdup_printf("lg:%s:%d", hid, dev);
    r->pct = -1; r->type = 0;
    int fn = feature(fd, dev, 0x0005);
    if (fn) {
        GString *s = g_string_new("");
        int cnt = hpp(fd, dev, fn, 0, NULL, 0, o) > 0 ? o[0] : 0;
        while ((int)s->len < cnt) {
            guint8 off = s->len;
            int n = hpp(fd, dev, fn, 1, &off, 1, o);
            if (n <= 0) break;
            for (int i = 0; i < n && (int)s->len < cnt; i++) g_string_append_c(s, o[i] ? o[i] : ' ');
        }
        r->name = g_strstrip(g_string_free(s, FALSE));
        if (hpp(fd, dev, fn, 2, NULL, 0, o) > 0) r->type = lg_type(o[0]);
    }
    if (!r->name || !*r->name) { g_free(r->name); r->name = g_strdup("Logitech"); }
    int f;
    if ((f = feature(fd, dev, 0x1004)) && hpp(fd, dev, f, 1, NULL, 0, o) >= 3) {       /* UNIFIED_BATTERY */
        r->pct = o[0];
        if (!r->pct) r->pct = o[1] & 8 ? 100 : o[1] & 4 ? 60 : o[1] & 2 ? 20 : o[1] & 1 ? 5 : -1;
        r->charging = o[2] == 1 || o[2] == 2;
    } else if ((f = feature(fd, dev, 0x1000)) && hpp(fd, dev, f, 0, NULL, 0, o) >= 3) { /* BATTERY_STATUS */
        r->pct = o[0];
        r->charging = o[2] == 1 || o[2] == 2 || o[2] == 4;
    } else if ((f = feature(fd, dev, 0x1001)) && hpp(fd, dev, f, 0, NULL, 0, o) >= 3) { /* BATTERY_VOLTAGE */
        int mv = o[0] << 8 | o[1];
        r->pct = CLAMP((mv - 3500) * 100 / (4100 - 3500), 0, 100);                      /* грубо: 3,5 В — 0, 4,1 В — 100 */
        r->charging = (o[2] & 0x80) != 0;
    }
    if (r->pct < 0 || r->type < 0) { g_free(r->key); g_free(r->name); g_free(r); return NULL; }
    r->seen = g_get_real_time() / G_USEC_PER_SEC;
    return r;
}

/* hidraw з HID++ від Logitech? (046D і звіт 0x10/0x11 в описі) */
static gboolean lg_node(const char *n) {
    char *ue = g_strdup_printf("/sys/class/hidraw/%s/device/uevent", n), *rd = g_strdup_printf("/sys/class/hidraw/%s/device/report_descriptor", n);
    char *u = NULL, *d = NULL;
    gsize len = 0;
    gboolean ok = g_file_get_contents(ue, &u, NULL, NULL) && strstr(u, ":0000046D:") &&
                  g_file_get_contents(rd, &d, &len, NULL);
    gboolean hp = FALSE;
    for (gsize i = 0; ok && i + 1 < len; i++) if ((guint8)d[i] == 0x85 && ((guint8)d[i + 1] == 0x10 || (guint8)d[i + 1] == 0x11)) hp = TRUE;
    g_free(ue); g_free(rd); g_free(u); g_free(d);
    return ok && hp;
}

/* Фізичний приймач, до якого належить hidraw: HID_PHYS без «/inputN» (usb-…-3.3) */
static char *lg_group(const char *n, gboolean *logi, int *kind) {
    char *ue = g_strdup_printf("/sys/class/hidraw/%s/device/uevent", n), *u = NULL, *g = NULL;
    *logi = FALSE; *kind = 0;
    if (g_file_get_contents(ue, &u, NULL, NULL) && strstr(u, ":0000046D:")) {
        *logi = TRUE;
        char *p = strstr(u, "HID_PHYS=");
        if (p) { p += 9; g = g_strndup(p, strcspn(p, "\n")); char *s = strstr(g, "/input"); if (s) *s = 0; }
        char *rd = g_strdup_printf("/sys/class/hidraw/%s/device/report_descriptor", n), *d = NULL;
        gsize len = 0;
        if (g_file_get_contents(rd, &d, &len, NULL) && len >= 4 && (guint8)d[0] == 0x05 && d[1] == 0x01 && (guint8)d[2] == 0x09)
            *kind = d[3] == 0x02 ? UT_MOUSE : d[3] == 0x06 ? UT_KEYBOARD : 0;   /* перша колекція: миша / клавіатура */
        g_free(rd); g_free(d);
    }
    g_free(ue); g_free(u);
    return g;
}

typedef struct { GPtrArray *res; gboolean denied; } LgScan;
static void lg_thread(GTask *t, gpointer src, gpointer data, GCancellable *c) {
    LgScan *s = g_new0(LgScan, 1);
    s->res = g_ptr_array_new();
    GDir *dir = g_dir_open("/sys/class/hidraw", 0, NULL);
    const char *n;
    /* приймачі без HID++ (прості, напр. C542): заряд не передають — лише «є миша / клавіатура» */
    GHashTable *hpp_groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *plain = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);   /* група → тип */
    while (dir && (n = g_dir_read_name(dir))) {
        gboolean logi; int kind;
        char *g = lg_group(n, &logi, &kind);
        if (g && lg_node(n)) g_hash_table_add(hpp_groups, g_strdup(g));
        else if (g && kind && !g_hash_table_contains(plain, g)) g_hash_table_insert(plain, g_strdup(g), GINT_TO_POINTER(kind));
        g_free(g);
    }
    GHashTableIter pit; gpointer pk, pv;
    g_hash_table_iter_init(&pit, plain);
    while (g_hash_table_iter_next(&pit, &pk, &pv)) {
        if (g_hash_table_contains(hpp_groups, pk)) continue;          /* це інтерфейс приймача з HID++ (Bolt…) */
        LgRes *r = g_new0(LgRes, 1);
        r->key = g_strdup_printf("lg:plain:%s", (char *)pk);
        r->type = GPOINTER_TO_INT(pv);
        r->name = g_strdup(r->type == UT_KEYBOARD ? "Клавіатура Logitech" : "Миша Logitech");
        r->unknown = TRUE;
        r->pct = 0;
        r->seen = g_get_real_time() / G_USEC_PER_SEC;
        g_ptr_array_add(s->res, r);
    }
    g_hash_table_unref(hpp_groups); g_hash_table_unref(plain);
    if (dir) g_dir_rewind(dir);
    while (dir && (n = g_dir_read_name(dir))) {
        if (!lg_node(n)) continue;
        char *dev = g_strdup_printf("/dev/%s", n);
        int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { if (errno == EACCES || errno == EPERM) s->denied = TRUE; g_free(dev); continue; }
        for (int i = 1; i <= 6; i++) { LgRes *r = lg_query(fd, i, n); if (r) g_ptr_array_add(s->res, r); }
        LgRes *r = lg_query(fd, 0xFF, n);        /* пристрій по кабелю / Bluetooth напряму */
        if (r) g_ptr_array_add(s->res, r);
        close(fd);
        g_free(dev);
    }
    if (dir) g_dir_close(dir);
    g_task_return_pointer(t, s, NULL);
}

static void lgres_free(gpointer x) { LgRes *r = x; g_free(r->key); g_free(r->name); g_free(r); }

static void lg_done(GObject *src, GAsyncResult *res, gpointer u) {
    lg_busy = FALSE;
    LgScan *s = g_task_propagate_pointer(G_TASK(res), NULL);
    if (!s) return;
    lg_denied = s->denied;
    GHashTableIter pi; gpointer pk2, pv2;                /* прості приймачі: нема в цьому скануванні — витягли */
    g_hash_table_iter_init(&pi, lg_last);
    while (g_hash_table_iter_next(&pi, &pk2, &pv2)) {
        if (!((LgRes *)pv2)->unknown) continue;
        gboolean still = FALSE;
        for (guint i = 0; i < s->res->len; i++) if (!g_strcmp0(((LgRes *)s->res->pdata[i])->key, pk2)) still = TRUE;
        if (!still) { g_hash_table_remove(devs, pk2); g_hash_table_iter_remove(&pi); }
    }
    for (guint i = 0; i < s->res->len; i++) {
        LgRes *r = s->res->pdata[i];
        g_hash_table_replace(lg_last, r->key, r);
    }
    g_ptr_array_unref(s->res);
    g_free(s);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, lg_last);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        LgRes *r = v;
        Dev *d = g_hash_table_lookup(devs, r->key);
        if (now - r->seen > LG_KEEP_S) {           /* давно не чути — прибрати */
            if (d) g_hash_table_remove(devs, r->key);
            g_hash_table_iter_remove(&it);
            continue;
        }
        if (!d) d = row_new(r->key);
        if (r->unknown) {                          /* заряд невідомий — замість відсотків порожньо */
            dev_show(d, 999, r->type, r->name, FALSE, "", "Простий приймач Logitech — заряд не передає");
            continue;
        }
        char *tip = now - r->seen > LG_EVERY_S + 30
            ? g_strdup_printf("%s\nСпить — заряд станом на %s", r->name, "останню відповідь")
            : g_strdup_printf("%s\nLogitech, через приймач", r->name);
        dev_show(d, r->pct, r->type, r->name, r->charging, NULL, tip);
        g_free(tip);
    }
    /* приймач є, а доступу немає — підказка замість заряду */
    Dev *hint = g_hash_table_lookup(devs, "lg:denied");
    if (lg_denied && !hint) hint = row_new("lg:denied");
    if (hint) dev_show(hint, lg_denied ? 1000 : -1, UT_MOUSE, "Logitech: немає доступу", FALSE, "?",
                       "Заряд мишки / клавіатури Logitech читається через /dev/hidraw,\n"
                       "а він доступний лише root. Встанови правило udev:\n"
                       "  sudo pacman -S solaar\n"
                       "і перепідключи приймач (або перезавантаж).");
    resort();
}

static gboolean lg_tick(gpointer d) {
    if (lg_busy) return G_SOURCE_CONTINUE;
    lg_busy = TRUE;
    GTask *t = g_task_new(NULL, NULL, lg_done, NULL);
    g_task_run_in_thread(t, lg_thread);
    g_object_unref(t);
    return G_SOURCE_CONTINUE;
}
static gboolean lg_tick_once(gpointer d) { lg_tick(NULL); return G_SOURCE_REMOVE; }
static void on_hidraw(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer d) {
    char *b = g_file_get_basename(f);
    if (g_str_has_prefix(b, "hidraw") && (ev == G_FILE_MONITOR_EVENT_CREATED || ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED))
        g_timeout_add_seconds(2, lg_tick_once, NULL);   /* приймач вставили / змінились права */
    g_free(b);
}

void battery_init(void) {
    if (!load_pos(&bat)) { bat.mright = 320; bat.mtop = 420; }
    bat.empty = TRUE;                              /* поки не знайшли — не показувати */
    card_init(&bat, "desktop-battery", "battery");
    ui_header(&bat, "\U000F0079", "Заряд пристроїв", NULL, NULL);   /* 󰁹 */
    list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_pack_start(GTK_BOX(bat.card), list, FALSE, FALSE, 0);
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(bat.win)));
    devs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, dev_free);
    lg_last = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, lgres_free);
    lg_tick(NULL);
    g_timeout_add_seconds(LG_EVERY_S, lg_tick, NULL);
    GFile *dv = g_file_new_for_path("/dev");
    GFileMonitor *mon = g_file_monitor_directory(dv, G_FILE_MONITOR_NONE, NULL, NULL);
    if (mon) g_signal_connect(mon, "changed", G_CALLBACK(on_hidraw), NULL);
    g_object_unref(dv);
    sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (!sys) return;
    g_dbus_connection_signal_subscribe(sys, UP_NAME, UP_NAME, "DeviceAdded", UP_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_added, NULL, NULL);
    g_dbus_connection_signal_subscribe(sys, UP_NAME, UP_NAME, "DeviceRemoved", UP_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_removed, NULL, NULL);
    g_bus_watch_name_on_connection(sys, UP_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE, on_owner, on_lost, NULL, NULL);
}
