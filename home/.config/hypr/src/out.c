/* ВИХІД ЗВУКУ — куди йде звук (див. desk.c)
 *
 *   [󰕾] Вихід звуку
 *   ┌──────────────────────────────────┐
 *   │ [значок] Назва пристрою        ▾ │  ← поле вибору: клік — розгорнути список
 *   └──────────────────────────────────┘
 *     [значок] Динаміки               ✓   ← пристрої; клік — звук іде туди
 *     [значок] Навушники                    (і все, що зараз грає, — теж), список згортається
 *
 *   Пристрої з'являються / зникають самі (підключили навушники, Bluetooth, HDMI) —
 *   події звукового сервера (PipeWire через PulseAudio), без опитування.
 *   Сховати / показати — desk.sh toggle output */
#include "desk.h"
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

Card outc = { .pos_name = "deskout.pos", .off_name = "output.off" };

typedef struct { char *name, *desc; const char *icon; GtkWidget *row, *check; gboolean seen; } Out;

static pa_context *ctx;
static pa_glib_mainloop *loop;
static GHashTable *outs;                 /* назва → Out* */
static char *cur;                        /* поточний вихід */
static GtkWidget *field, *f_icon, *f_name, *f_sub, *f_arrow, *list, *list_box;
static gboolean open_list;

static const char *icon_for(const pa_sink_info *i) {
    const char *ff = pa_proplist_gets(i->proplist, "device.form_factor");
    const char *bus = pa_proplist_gets(i->proplist, "device.bus");
    const char *n = i->name ? i->name : "";
    if (ff && strstr(ff, "headset")) return "\U000F02CE";                                   /* 󰋎 */
    if ((ff && strstr(ff, "headphone")) || (bus && !strcmp(bus, "bluetooth"))) return "\U000F02CB";   /* 󰋋 */
    if (strstr(n, "hdmi") || strstr(n, "HDMI") || (ff && strstr(ff, "tv"))) return "\U000F0379";      /* 󰍹 */
    return "\U000F04C3";                                                                      /* 󰓃 */
}
static const char *kind_for(const pa_sink_info *i) {
    const char *bus = pa_proplist_gets(i->proplist, "device.bus");
    const char *n = i->name ? i->name : "";
    if (bus && !strcmp(bus, "bluetooth")) return "Bluetooth";
    if (bus && !strcmp(bus, "usb")) return "USB";
    if (strstr(n, "hdmi") || strstr(n, "HDMI")) return "HDMI / монітор";
    return "вбудований";
}

static void shrink(void) { gtk_window_resize(GTK_WINDOW(outc.win), 1, 1); }

static void show_current(void) {
    Out *o = cur ? g_hash_table_lookup(outs, cur) : NULL;
    gtk_label_set_text(GTK_LABEL(f_icon), o ? o->icon : "\U000F0581");
    gtk_label_set_text(GTK_LABEL(f_name), o ? o->desc : "Немає виходу звуку");
    gtk_widget_set_tooltip_text(field, o ? o->desc : NULL);
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, outs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Out *x = v;
        gboolean on = cur && !strcmp(x->name, cur);
        gtk_label_set_text(GTK_LABEL(x->check), on ? "\U000F012C" : "");                    /* 󰄬 */
        GtkStyleContext *sc = gtk_widget_get_style_context(x->row);
        if (on) gtk_style_context_add_class(sc, "active"); else gtk_style_context_remove_class(sc, "active");
    }
    gtk_label_set_text(GTK_LABEL(f_sub), g_hash_table_size(outs) > 1 ? "натисни, щоб вибрати інший" : "інших виходів немає");
}

static void set_open(gboolean o) {
    open_list = o;
    gtk_widget_set_visible(list_box, o);
    gtk_label_set_text(GTK_LABEL(f_arrow), o ? "\U000F0143" : "\U000F0140");              /* 󰅃 / 󰅀 */
    GtkStyleContext *sc = gtk_widget_get_style_context(field);
    if (o) gtk_style_context_add_class(sc, "open"); else gtk_style_context_remove_class(sc, "open");
    shrink();
}
static void on_field(GtkButton *b, gpointer d) { set_open(!open_list); }

static void on_move_list(pa_context *c, const pa_sink_input_info *i, int eol, void *d) {
    if (eol || !i || !cur) return;
    pa_operation *op = pa_context_move_sink_input_by_name(c, i->index, cur, NULL, NULL);
    if (op) pa_operation_unref(op);
}
static void on_pick(GtkButton *b, Out *o) {
    if (!ctx) return;
    g_free(cur);
    cur = g_strdup(o->name);
    pa_operation *op = pa_context_set_default_sink(ctx, o->name, NULL, NULL);
    if (op) pa_operation_unref(op);
    op = pa_context_get_sink_input_info_list(ctx, on_move_list, NULL);   /* усе, що грає, — туди ж */
    if (op) pa_operation_unref(op);
    show_current();
    set_open(FALSE);
}

static void out_free(gpointer p) {
    Out *o = p;
    if (o->row) gtk_widget_destroy(o->row);
    g_free(o->name); g_free(o->desc); g_free(o);
}

static void got_sink(pa_context *c, const pa_sink_info *i, int eol, void *d) {
    if (eol) {                                     /* кінець списку: зниклі — прибрати */
        GHashTableIter it; gpointer k, v;
        g_hash_table_iter_init(&it, outs);
        while (g_hash_table_iter_next(&it, &k, &v)) if (!((Out *)v)->seen) g_hash_table_iter_remove(&it);
        show_current();
        shrink();
        return;
    }
    if (!i || !i->name || !strcmp(i->name, "auto_null")) return;
    Out *o = g_hash_table_lookup(outs, i->name);
    if (!o) {
        o = g_new0(Out, 1);
        o->name = g_strdup(i->name);
        o->row = gtk_button_new();
        add_class(o->row, "o-row");
        gtk_widget_set_can_focus(o->row, FALSE);
        g_signal_connect(o->row, "clicked", G_CALLBACK(on_pick), o);
        gtk_box_pack_start(GTK_BOX(list), o->row, FALSE, FALSE, 0);
        g_hash_table_insert(outs, o->name, o);
    }
    g_free(o->desc);
    o->desc = g_strdup(i->description ? i->description : i->name);
    o->icon = icon_for(i);
    o->seen = TRUE;
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(o->row));
    if (child) gtk_widget_destroy(child);
    GtkWidget *box = ui_row_box(ui_lead(o->icon), ui_text(o->desc, kind_for(i), NULL, NULL));
    o->check = gtk_label_new("");
    add_class(o->check, "o-check");
    gtk_widget_set_valign(o->check, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(box), o->check, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(o->row), box);
    gtk_widget_show_all(o->row);
}
static void refresh_list(pa_context *c) {
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, outs);
    while (g_hash_table_iter_next(&it, &k, &v)) ((Out *)v)->seen = FALSE;
    pa_operation_unref(pa_context_get_sink_info_list(c, got_sink, NULL));
}
static void got_server(pa_context *c, const pa_server_info *i, void *d) {
    if (!i) return;
    g_free(cur);
    cur = g_strdup(i->default_sink_name);
    show_current();
}
static void on_event(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *d) {
    int fac = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    if (fac == PA_SUBSCRIPTION_EVENT_SINK) refresh_list(c);
    else if (fac == PA_SUBSCRIPTION_EVENT_SERVER) pa_operation_unref(pa_context_get_server_info(c, got_server, NULL));
}
static gboolean connect_later(gpointer d);
static void on_state(pa_context *c, void *d) {
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        pa_context_set_subscribe_callback(c, on_event, NULL);
        pa_operation_unref(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL));
        pa_operation_unref(pa_context_get_server_info(c, got_server, NULL));
        refresh_list(c);
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        pa_context_unref(ctx);
        ctx = NULL;
        g_hash_table_remove_all(outs);
        show_current();
        g_timeout_add_seconds(3, connect_later, NULL);
        break;
    default: break;
    }
}
static gboolean connect_later(gpointer d) {
    pa_proplist *p = pa_proplist_new();
    pa_proplist_sets(p, PA_PROP_APPLICATION_NAME, "Вихід звуку (desk)");
    ctx = pa_context_new_with_proplist(pa_glib_mainloop_get_api(loop), "desk-output", p);
    pa_proplist_free(p);
    pa_context_set_state_callback(ctx, on_state, NULL);
    pa_context_connect(ctx, NULL, PA_CONTEXT_NOFAIL, NULL);
    return G_SOURCE_REMOVE;
}

void out_init(void) {
    if (!load_pos(&outc)) { outc.mright = CAL_RIGHT + CARD_W + GRID; outc.mtop = CAL_TOP; }
    card_init(&outc, "desktop-output", "output");

    field = gtk_button_new();                      /* поле вибору */
    add_class(field, "ui-select");
    gtk_widget_set_can_focus(field, FALSE);
    GtkWidget *col = ui_text("", "", &f_name, &f_sub);
    f_icon = ui_lead("");
    GtkWidget *box = ui_row_box(f_icon, col);
    f_arrow = gtk_label_new("");
    add_class(f_arrow, "ui-select-arrow");
    gtk_widget_set_valign(f_arrow, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(box), f_arrow, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(field), box);
    g_signal_connect(field, "clicked", G_CALLBACK(on_field), NULL);
    gtk_box_pack_start(GTK_BOX(outc.card), field, FALSE, FALSE, 0);

    list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);   /* розгорнутий список */
    add_class(list_box, "ui-list");
    add_class(list_box, "ui-select-list");
    list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(list_box), list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outc.card), list_box, FALSE, FALSE, 0);

    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(outc.win)));
    gtk_widget_set_no_show_all(list_box, TRUE);
    set_open(FALSE);
    outs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, out_free);
    loop = pa_glib_mainloop_new(NULL);
    connect_later(NULL);
}
