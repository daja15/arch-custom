/* СИСТЕМА — процесор, температура, пам'ять, диск, живлення (див. desk.c)
 *
 *   • процесор: завантаження, середня частота й графік за 2 хвилини;
 *   • температура пакета процесора (coretemp) — у тому ж рядку, що й процесор;
 *   • графічний чип (Intel i915): завантаження з лічильника сну RC6 і частота;
 *   • пам'ять і диск «/»;
 *   • живлення: скільки ват бере ноутбук від батареї й на скільки її вистачить
 *     (від мережі — швидкість заряджання);
 *   • кнопка «btop» — докладний монітор у терміналі.
 *
 *   Економія: дані — з /proc і /sys раз на 2 с і ЛИШЕ поки картку видно.
 *   Закрита непрозорим вікном — стоїть (0 навантаження); крізь kitty видно — працює
 *   (hyprwatch: події Hyprland, без опитування). */
#include "desk.h"
#include <sys/statvfs.h>

#define EVERY_S   2              /* оновлення, с */
#define HIST      60             /* точок графіка (60 × 2 с = 2 хв) */

Card sysm = { .pos_name = "desksys.pos", .off_name = "system.off" };

typedef struct { GtkWidget *row, *val, *bar; } Meter;
static Meter m_cpu, m_gpu, m_ram, m_dsk;
static GtkWidget *lbl_up, *graph, *lbl_pw, *lbl_pwsub, *bar_bat, *row_pw;
static double hist[HIST];
static int hist_n;
static guint timer;
static gboolean seen = TRUE;                     /* картку видно (не закрита вікном) */
static char *temp_path, *bat_dir, *gpu_dir, *gt_dir;
static unsigned long long prev_idle, prev_total;
static long long prev_rc6 = -1;                  /* мс сну графіки (RC6) на минулому замірі */
static gint64 prev_rc6_t;

/* ── читання /proc і /sys ── */
static char *slurp(const char *path) { char *s = NULL; g_file_get_contents(path, &s, NULL, NULL); return s; }
static long long read_ll(const char *dir, const char *name) {
    char *p = g_build_filename(dir, name, NULL), *s = slurp(p);
    long long v = s ? g_ascii_strtoll(s, NULL, 10) : -1;
    g_free(s); g_free(p);
    return v;
}

static double cpu_usage(void) {
    char *s = slurp("/proc/stat");
    unsigned long long v[8] = { 0 };
    if (!s || sscanf(s, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                     &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 8) { g_free(s); return 0; }
    g_free(s);
    unsigned long long idle = v[3] + v[4], total = 0;
    for (int i = 0; i < 8; i++) total += v[i];
    double u = prev_total && total > prev_total ? 1.0 - (double)(idle - prev_idle) / (total - prev_total) : 0;
    prev_idle = idle; prev_total = total;
    return CLAMP(u, 0, 1);
}

static double cpu_ghz(void) {                    /* середня частота всіх ядер */
    double sum = 0; int n = 0;
    GDir *d = g_dir_open("/sys/devices/system/cpu", 0, NULL);
    const char *e;
    while (d && (e = g_dir_read_name(d))) {
        if (!g_str_has_prefix(e, "cpu") || !g_ascii_isdigit(e[3])) continue;
        char *dir = g_strdup_printf("/sys/devices/system/cpu/%s/cpufreq", e);
        long long khz = read_ll(dir, "scaling_cur_freq");
        g_free(dir);
        if (khz > 0) { sum += khz; n++; }
    }
    if (d) g_dir_close(d);
    return n ? sum / n / 1e6 : 0;
}

/* Завантаження графіки: частка часу НЕ у сні RC6 між замірами (без root, без intel_gpu_top) */
static double gpu_busy(void) {
    if (!gt_dir) return -1;
    long long rc6 = read_ll(gt_dir, "rc6_residency_ms");
    gint64 now = g_get_monotonic_time();
    double u = -1;
    if (rc6 >= 0 && prev_rc6 >= 0 && now > prev_rc6_t)
        u = CLAMP(1.0 - (rc6 - prev_rc6) / ((now - prev_rc6_t) / 1000.0), 0, 1);
    prev_rc6 = rc6; prev_rc6_t = now;
    return u;
}

static void find_sensors(void) {                 /* coretemp → temp1 (пакет), перша батарея BAT*, графіка cardN з gt/gt0 */
    GDir *d = g_dir_open("/sys/class/hwmon", 0, NULL);
    const char *e;
    while (d && !temp_path && (e = g_dir_read_name(d))) {
        char *np = g_strdup_printf("/sys/class/hwmon/%s/name", e), *name = slurp(np);
        if (name && !strcmp(g_strstrip(name), "coretemp")) temp_path = g_strdup_printf("/sys/class/hwmon/%s", e);
        g_free(name); g_free(np);
    }
    if (d) g_dir_close(d);
    d = g_dir_open("/sys/class/drm", 0, NULL);
    while (d && !gpu_dir && (e = g_dir_read_name(d))) {
        if (!g_str_has_prefix(e, "card") || strchr(e, '-')) continue;   /* card1, а не card1-eDP-1 */
        char *dir = g_strdup_printf("/sys/class/drm/%s", e), *gt = g_build_filename(dir, "gt", "gt0", NULL);
        if (g_file_test(gt, G_FILE_TEST_IS_DIR)) { gpu_dir = dir; gt_dir = gt; }
        else { g_free(dir); g_free(gt); }
    }
    if (d) g_dir_close(d);
    d = g_dir_open("/sys/class/power_supply", 0, NULL);
    while (d && !bat_dir && (e = g_dir_read_name(d)))
        if (g_str_has_prefix(e, "BAT")) bat_dir = g_strdup_printf("/sys/class/power_supply/%s", e);
    if (d) g_dir_close(d);
}

/* ── показ ── */
static void meter_set(Meter *m, double frac, const char *text, double warn, double crit) {
    gtk_label_set_text(GTK_LABEL(m->val), text);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(m->bar), CLAMP(frac, 0, 1));
    GtkStyleContext *sc = gtk_widget_get_style_context(m->row);
    if (frac >= crit) gtk_style_context_add_class(sc, "crit"); else gtk_style_context_remove_class(sc, "crit");
    if (frac >= warn && frac < crit) gtk_style_context_add_class(sc, "warn"); else gtk_style_context_remove_class(sc, "warn");
}

static void power_update(void) {
    if (!bat_dir) { gtk_label_set_text(GTK_LABEL(lbl_pw), "—"); gtk_label_set_text(GTK_LABEL(lbl_pwsub), "батареї немає"); return; }
    char *sp = g_build_filename(bat_dir, "status", NULL), *st = slurp(sp);
    g_free(sp);
    if (st) g_strstrip(st);
    long long pw = read_ll(bat_dir, "power_now");                 /* мкВт */
    if (pw < 0) {                                                 /* деякі батареї — струм × напруга */
        long long cur = read_ll(bat_dir, "current_now"), volt = read_ll(bat_dir, "voltage_now");
        pw = cur >= 0 && volt >= 0 ? cur * volt / 1000000 : -1;
    }
    long long en = read_ll(bat_dir, "energy_now"), full = read_ll(bat_dir, "energy_full");
    long long cap = read_ll(bat_dir, "capacity");                 /* заряд, % */
    char buf[64], sub[96];
    if (pw >= 0 && cap >= 0) g_snprintf(buf, sizeof buf, "%lld%% · %.1f Вт", cap, pw / 1e6);
    else if (pw >= 0)        g_snprintf(buf, sizeof buf, "%.1f Вт", pw / 1e6);
    else if (cap >= 0)       g_snprintf(buf, sizeof buf, "%lld%%", cap);
    else g_strlcpy(buf, "—", sizeof buf);
    /* смужка — заряд; колір навпаки: мало заряду — жовте (≤ 30 %), червоне (≤ 15 %), лише від батареї */
    gboolean dis = st && !strcmp(st, "Discharging");
    gtk_level_bar_set_value(GTK_LEVEL_BAR(bar_bat), cap >= 0 ? CLAMP(cap / 100.0, 0, 1) : 0);
    GtkStyleContext *sc = gtk_widget_get_style_context(row_pw);
    if (dis && cap >= 0 && cap <= 15) gtk_style_context_add_class(sc, "crit"); else gtk_style_context_remove_class(sc, "crit");
    if (dis && cap > 15 && cap <= 30) gtk_style_context_add_class(sc, "warn"); else gtk_style_context_remove_class(sc, "warn");
    if (st && !strcmp(st, "Discharging")) {
        if (pw > 100000 && en > 0) {
            int min = (int)(en / (double)pw * 60);
            g_snprintf(sub, sizeof sub, "від батареї · ще %d год %02d хв", min / 60, min % 60);
        } else g_strlcpy(sub, "від батареї", sizeof sub);
    } else if (st && !strcmp(st, "Charging")) {
        if (pw > 100000 && en >= 0 && full > en) {
            int min = (int)((full - en) / (double)pw * 60);
            g_snprintf(sub, sizeof sub, "заряджається · повна за %d год %02d хв", min / 60, min % 60);
        } else g_strlcpy(sub, "заряджається", sizeof sub);
    } else {
        g_strlcpy(sub, "від мережі", sizeof sub);
        if (pw <= 0) { if (cap >= 0) g_snprintf(buf, sizeof buf, "%lld%%", cap); else g_strlcpy(buf, "мережа", sizeof buf); }
    }
    gtk_label_set_text(GTK_LABEL(lbl_pw), buf);
    gtk_label_set_text(GTK_LABEL(lbl_pwsub), sub);
    g_free(st);
}

static gboolean update(gpointer d) {
    char buf[64];
    double u = cpu_usage();
    memmove(hist, hist + 1, sizeof(double) * (HIST - 1));
    hist[HIST - 1] = u;
    hist_n = MIN(hist_n + 1, HIST);
    gtk_widget_queue_draw(graph);
    /* Процесор: завантаження · частота · температура. Колір — гірше з двох:
     * завантаження (70 % жовте, 90 % червоне) чи нагрів (65 °C жовте, 79 °C червоне) */
    long long t = temp_path ? read_ll(temp_path, "temp1_input") : -1;
    if (t >= 0) g_snprintf(buf, sizeof buf, "%d%% · %.1f ГГц · %d°", (int)round(u * 100), cpu_ghz(), (int)(t / 1000));
    else        g_snprintf(buf, sizeof buf, "%d%% · %.1f ГГц", (int)round(u * 100), cpu_ghz());
    double heat = t >= 0 ? t / 1000.0 : 0;
    double sev = MAX(u >= 0.9 ? 1.0 : u >= 0.7 ? 0.5 : 0, heat >= 79 ? 1.0 : heat >= 65 ? 0.5 : 0);
    meter_set(&m_cpu, sev, buf, 0.5, 1.0);                  /* клас warn/crit — за гіршим … */
    gtk_level_bar_set_value(GTK_LEVEL_BAR(m_cpu.bar), u);   /* … а смужка — завантаження */

    /* Графіка: завантаження · поточна частота */
    double g = gpu_busy();
    long long mhz = gpu_dir ? read_ll(gpu_dir, "gt_act_freq_mhz") : -1;
    if (g >= 0) {
        if (mhz > 0) g_snprintf(buf, sizeof buf, "%d%% · %lld МГц", (int)round(g * 100), mhz);
        else         g_snprintf(buf, sizeof buf, "%d%% · спить", (int)round(g * 100));
        meter_set(&m_gpu, g, buf, 0.7, 0.9);
    } else meter_set(&m_gpu, 0, gt_dir ? "…" : "—", 2, 2);

    char *mi = slurp("/proc/meminfo");
    long long tot = 0, avail = 0;
    char **lines = mi ? g_strsplit(mi, "\n", -1) : NULL;
    for (char **p = lines; p && *p; p++) {
        sscanf(*p, "MemTotal: %lld", &tot);
        sscanf(*p, "MemAvailable: %lld", &avail);
    }
    g_strfreev(lines);
    g_free(mi);
    if (tot > 0) {
        g_snprintf(buf, sizeof buf, "%.1f / %.1f ГБ", (tot - avail) / 1048576.0, tot / 1048576.0);
        meter_set(&m_ram, (double)(tot - avail) / tot, buf, 0.75, 0.9);
    }

    struct statvfs fs;
    if (statvfs("/", &fs) == 0 && fs.f_blocks) {
        double total = (double)fs.f_blocks * fs.f_frsize, used = total - (double)fs.f_bavail * fs.f_frsize;
        g_snprintf(buf, sizeof buf, "%.0f / %.0f ГБ", used / 1e9, total / 1e9);
        meter_set(&m_dsk, used / total, buf, 0.85, 0.95);
    }
    power_update();

    double up = 0;
    char *us = slurp("/proc/uptime");
    if (us) up = g_ascii_strtod(us, NULL);
    g_free(us);
    int h = (int)(up / 3600), mnt = (int)(up / 60) % 60;
    if (h >= 24) g_snprintf(buf, sizeof buf, "увімкнено %d д %d год", h / 24, h % 24);
    else         g_snprintf(buf, sizeof buf, "увімкнено %d год %02d хв", h, mnt);
    gtk_label_set_text(GTK_LABEL(lbl_up), buf);
    return G_SOURCE_CONTINUE;
}

/* Таймер — лише поки картку показано й видно */
static void retime(void) {
    gboolean want = gtk_widget_get_visible(sysm.win) && seen;
    if (want && !timer) { update(NULL); timer = g_timeout_add_seconds(EVERY_S, update, NULL); }
    if (!want && timer) { g_source_remove(timer); timer = 0; }
}
static gboolean recheck(gpointer d) {
    if (gtk_widget_get_visible(sysm.win)) seen = card_visible(&sysm);
    retime();
    return G_SOURCE_REMOVE;
}
static void recheck_soon(void) { g_timeout_add(150, recheck, NULL); }

/* Графік процесора: заливка під лінією, акцентний колір */
static gboolean on_graph(GtkWidget *w, cairo_t *cr, gpointer d) {
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    cairo_set_source_rgba(cr, TH_LINE_RGB, 0.6);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 0, H - 0.5); cairo_line_to(cr, W, H - 0.5);
    cairo_stroke(cr);
    if (hist_n < 2) return FALSE;
    double step = W / (HIST - 1);
    int first = HIST - hist_n;
    cairo_move_to(cr, first * step, H);
    for (int i = first; i < HIST; i++) cairo_line_to(cr, i * step, H - 1 - hist[i] * (H - 3));
    cairo_line_to(cr, W, H);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, TH_ACCENT_RGB, 0.18);
    cairo_fill(cr);
    for (int i = first; i < HIST; i++) {
        double x = i * step, y = H - 1 - hist[i] * (H - 3);
        if (i == first) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
    }
    cairo_set_source_rgb(cr, TH_ACCENT_RGB);
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
    return FALSE;
}

static void meter_new(Meter *m, const char *icon, const char *name) {
    /* [значок] Назва ········ значення
     *          ━━━━━━━━━━━━━━━━━━━━━━   смужка — під текстом, від тієї ж лінії */
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *nm = gtk_label_new(name);
    add_class(nm, "s-name");
    m->val = gtk_label_new("—");
    add_class(m->val, "s-val");
    gtk_box_pack_start(GTK_BOX(top), nm, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top), m->val, FALSE, FALSE, 0);
    m->bar = gtk_level_bar_new_for_interval(0, 1);
    add_class(m->bar, "s-meter");
    gtk_box_pack_start(GTK_BOX(col), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), m->bar, FALSE, FALSE, 0);
    m->row = ui_row_box(ui_lead(icon), col);
    add_class(m->row, "s-row");
    gtk_box_pack_start(GTK_BOX(sysm.card), m->row, FALSE, FALSE, 0);
}

static void on_btop(GtkButton *b, gpointer d) {
    char *argv[] = { "kitty", "--single-instance", "--instance-group=term", "btop", NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

void sys_init(void) {
    if (!load_pos(&sysm)) { sysm.mright = 1920 - 60 - CARD_W; sysm.mtop = CAL_TOP; }   /* ліворуч угорі */
    sysm.moved = recheck_soon;
    sysm.shown = recheck_soon;
    sysm.hidden = retime;
    card_init(&sysm, "desktop-system", "system");
    gtk_widget_set_size_request(sysm.card, SYS_W, -1);   /* ширша за інші картки */
    find_sensors();

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);   /* без назви: час роботи ········ [btop] */
    lbl_up = gtk_label_new("");
    add_class(lbl_up, "s-sub");
    gtk_label_set_xalign(GTK_LABEL(lbl_up), 0);
    gtk_widget_set_valign(lbl_up, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(head), lbl_up, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sysm.card), head, FALSE, FALSE, 0);
    GtkWidget *btn = gtk_button_new_with_label("btop");
    add_class(btn, "s-btn");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    gtk_widget_set_can_focus(btn, FALSE);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_btop), NULL);
    gtk_box_pack_end(GTK_BOX(head), btn, FALSE, FALSE, 0);

    graph = gtk_drawing_area_new();
    add_class(graph, "s-graph");
    gtk_widget_set_size_request(graph, -1, 20);   /* картка — рівно з календарем (cal.c): графік бере решту висоти */
    gtk_widget_set_margin_start(graph, 8);         /* полотно не бере відступи з CSS */
    gtk_widget_set_margin_end(graph, 8);
    gtk_widget_set_margin_top(graph, 4);
    g_signal_connect(graph, "draw", G_CALLBACK(on_graph), NULL);
    gtk_box_pack_start(GTK_BOX(sysm.card), graph, TRUE, TRUE, 0);

    meter_new(&m_cpu, "\xef\x8b\x9b", "Процесор");
    meter_new(&m_gpu, "\xf3\xb0\xa2\xae", "Графіка");   /* значок відеокарти */
    meter_new(&m_ram, "\xee\xbf\x85", "Пам'ять");
    meter_new(&m_dsk, "\xef\x82\xa0", "Диск");

    GtkWidget *pcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);   /* живлення: заряд · ват, смужка заряду, підпис (від батареї · ще …) */
    GtkWidget *ptop = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *nm = gtk_label_new("Живлення");
    add_class(nm, "s-name");
    lbl_pw = gtk_label_new("—");
    add_class(lbl_pw, "s-val");
    gtk_box_pack_start(GTK_BOX(ptop), nm, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(ptop), lbl_pw, FALSE, FALSE, 0);
    lbl_pwsub = gtk_label_new("");
    add_class(lbl_pwsub, "s-sub");
    gtk_label_set_xalign(GTK_LABEL(lbl_pwsub), 0);
    bar_bat = gtk_level_bar_new_for_interval(0, 1);
    add_class(bar_bat, "s-meter");
    gtk_box_pack_start(GTK_BOX(pcol), ptop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pcol), bar_bat, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pcol), lbl_pwsub, FALSE, FALSE, 0);
    row_pw = ui_row_box(ui_lead("\xef\x83\xa7"), pcol);
    add_class(row_pw, "s-row");
    gtk_box_pack_start(GTK_BOX(sysm.card), row_pw, FALSE, FALSE, 0);

    cpu_usage();                                    /* перший замір — точка відліку */
    gpu_busy();
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(sysm.win)));
    hw_listen(recheck);
}
