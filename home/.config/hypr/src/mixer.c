/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  МІКШЕР ЗВУКУ — віджет робочого столу (частина desk)         │
 * │  Стовпчики: перший — загальна гучність (поточний вихід       │
 * │  звуку), далі — кожна програма, що зараз грає. У стовпчику:  │
 * │  коротка назва (повна — у підказці), вертикальний повзунок   │
 * │  (вгору — гучніше, до 100%) і «без звуку» знизу.             │
 * │  Програма почала грати — стовпчик з'являється сам, замовкла  │
 * │  — зникає: події звукового сервера (PipeWire через           │
 * │  сумісність з PulseAudio), без опитування.                   │
 * │  Картка — по центру екрана: росте в обидва боки, не з'їжджає │
 * │  Сховати / показати — desk.sh toggle mixer                   │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include "desk.h"
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

#define MAX_VOL 1.0            /* повзунок до 100% */
#define SLIDER_H 92            /* висота повзунка (px) */
#define NAME_CH  8             /* коротка назва — стільки літер */

Card mix = { .pos_name = "deskmixer.pos", .off_name = "mixer.off", .mright = CAL_RIGHT, .centered = TRUE };

typedef struct {
    uint32_t idx;              /* номер потоку (PA_INVALID_INDEX — загальна гучність) */
    GtkWidget *row, *name, *scale, *mute;
    pa_cvolume vol;
    gboolean updating;         /* міняємо повзунок самі — не слати назад */
} Row;

static pa_context *ctx;
static GtkWidget *list;
static Row master;
static char *sink_name;        /* поточний вихід звуку */
static GHashTable *rows;       /* idx → Row* */

static void shrink(void) { gtk_window_resize(GTK_WINDOW(mix.win), 1, 1); }   /* картка під вміст */

/* ───── повзунок / кнопка → звуковий сервер */
static void on_scale(GtkRange *r, Row *row) {
    if (row->updating || !ctx) return;
    pa_volume_t v = (pa_volume_t)(gtk_range_get_value(r) * PA_VOLUME_NORM);
    pa_cvolume cv = row->vol;
    pa_cvolume_set(&cv, row->vol.channels ? row->vol.channels : 2, v);
    pa_operation *op = row->idx == PA_INVALID_INDEX
        ? pa_context_set_sink_volume_by_name(ctx, sink_name, &cv, NULL, NULL)
        : pa_context_set_sink_input_volume(ctx, row->idx, &cv, NULL, NULL);
    if (op) pa_operation_unref(op);
}
static void on_mute(GtkToggleButton *b, Row *row) {
    if (row->updating || !ctx) return;
    int m = gtk_toggle_button_get_active(b);
    pa_operation *op = row->idx == PA_INVALID_INDEX
        ? pa_context_set_sink_mute_by_name(ctx, sink_name, m, NULL, NULL)
        : pa_context_set_sink_input_mute(ctx, row->idx, m, NULL, NULL);
    if (op) pa_operation_unref(op);
}

static void row_build(Row *r) {
    r->row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    add_class(r->row, "m-col");
    r->name = gtk_label_new("");
    add_class(r->name, "m-name");
    gtk_label_set_ellipsize(GTK_LABEL(r->name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(r->name), NAME_CH);
    gtk_label_set_width_chars(GTK_LABEL(r->name), NAME_CH);
    r->scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0, MAX_VOL, 0.01);
    gtk_range_set_inverted(GTK_RANGE(r->scale), TRUE);                  /* вгору — гучніше */
    gtk_scale_set_draw_value(GTK_SCALE(r->scale), FALSE);
    gtk_widget_set_size_request(r->scale, -1, SLIDER_H);
    gtk_widget_set_halign(r->scale, GTK_ALIGN_CENTER);
    gtk_widget_set_can_focus(r->scale, FALSE);
    r->mute = gtk_toggle_button_new_with_label("\U000F075F");          /* 󰝟 без звуку */
    add_class(r->mute, "m-mute");
    gtk_widget_set_can_focus(r->mute, FALSE);
    gtk_widget_set_tooltip_text(r->mute, "Без звуку");
    gtk_widget_set_halign(r->mute, GTK_ALIGN_CENTER);
    g_signal_connect(r->scale, "value-changed", G_CALLBACK(on_scale), r);
    g_signal_connect(r->mute, "toggled", G_CALLBACK(on_mute), r);
    gtk_box_pack_start(GTK_BOX(r->row), r->name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(r->row), r->scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(r->row), r->mute, FALSE, FALSE, 0);
}

static void row_set(Row *r, const char *name, const pa_cvolume *cv, int muted) {
    r->updating = TRUE;
    r->vol = *cv;
    gtk_label_set_text(GTK_LABEL(r->name), name);
    gtk_widget_set_tooltip_text(r->row, name);                          /* повна назва — при наведенні */
    if (!gtk_widget_get_state_flags(r->scale) || !(gtk_widget_get_state_flags(r->scale) & GTK_STATE_FLAG_ACTIVE))
        gtk_range_set_value(GTK_RANGE(r->scale), (double)pa_cvolume_avg(cv) / PA_VOLUME_NORM);   /* не смикати, поки тягнеш */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->mute), muted);
    r->updating = FALSE;
}


/* ───── звуковий сервер → рядки */
static void got_input(pa_context *c, const pa_sink_input_info *i, int eol, void *d) {
    if (eol || !i) return;
    if (pa_proplist_gets(i->proplist, "pulse.peak_detect")) return;   /* службові потоки (вимірювачі рівня) */
    Row *r = g_hash_table_lookup(rows, GUINT_TO_POINTER(i->index));
    const char *app = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME);
    const char *media = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME);
    if (!r) {
        r = g_new0(Row, 1);
        r->idx = i->index;
        row_build(r);
        gtk_box_pack_start(GTK_BOX(list), r->row, FALSE, FALSE, 0);
        gtk_widget_show_all(r->row);
        g_hash_table_insert(rows, GUINT_TO_POINTER(i->index), r);
    }
    char *name = media && app && g_strcmp0(media, app) != 0 ? g_strdup_printf("%s · %s", app, media)
                                                            : g_strdup(app ? app : media ? media : "Звук");
    row_set(r, name, &i->volume, i->mute);
    gtk_label_set_text(GTK_LABEL(r->name), app ? app : "Звук");          /* над повзунком — коротко */
    g_free(name);
}

static void got_sink(pa_context *c, const pa_sink_info *i, int eol, void *d) {
    if (eol || !i) return;
    row_set(&master, i->description ? i->description : "Звук", &i->volume, i->mute);
    gtk_label_set_text(GTK_LABEL(master.name), "Усе");                  /* над повзунком; пристрій — у підказці */
}

static void got_server(pa_context *c, const pa_server_info *i, void *d) {
    if (!i) return;
    g_free(sink_name);
    sink_name = g_strdup(i->default_sink_name);
    pa_operation_unref(pa_context_get_sink_info_by_name(c, sink_name, got_sink, NULL));
}

static void on_event(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *d) {
    int fac = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK, type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    if (fac == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {                       /* програма замовкла — прибрати рядок */
            Row *r = g_hash_table_lookup(rows, GUINT_TO_POINTER(idx));
            if (r) { gtk_widget_destroy(r->row); g_hash_table_remove(rows, GUINT_TO_POINTER(idx)); g_free(r); shrink(); }
        } else pa_operation_unref(pa_context_get_sink_input_info(c, idx, got_input, NULL));
    } else if (fac == PA_SUBSCRIPTION_EVENT_SINK && sink_name) {
        pa_operation_unref(pa_context_get_sink_info_by_name(c, sink_name, got_sink, NULL));
    } else if (fac == PA_SUBSCRIPTION_EVENT_SERVER) {                    /* змінився вихід звуку */
        pa_operation_unref(pa_context_get_server_info(c, got_server, NULL));
    }
}

static gboolean connect_later(gpointer d);
static void free_value(gpointer k, gpointer v, gpointer d) { g_free(v); }
static void destroy_child(GtkWidget *w, gpointer d) { gtk_widget_destroy(w); }
static void on_state(pa_context *c, void *d) {
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        pa_context_set_subscribe_callback(c, on_event, NULL);
        pa_operation_unref(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT |
                                                   PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL));
        pa_operation_unref(pa_context_get_server_info(c, got_server, NULL));
        pa_operation_unref(pa_context_get_sink_input_info_list(c, got_input, NULL));
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:                                          /* сервер перезапустився — пізніше ще раз */
        pa_context_unref(ctx);
        ctx = NULL;
        gtk_container_foreach(GTK_CONTAINER(list), destroy_child, NULL);                   /* рядки програм */
        g_hash_table_foreach(rows, (GHFunc)free_value, NULL);
        g_hash_table_remove_all(rows);
        g_timeout_add_seconds(3, connect_later, NULL);
        break;
    default: break;
    }
}

static pa_glib_mainloop *loop;
static gboolean connect_later(gpointer d) {
    pa_proplist *p = pa_proplist_new();
    pa_proplist_sets(p, PA_PROP_APPLICATION_NAME, "Мікшер (desk)");
    ctx = pa_context_new_with_proplist(pa_glib_mainloop_get_api(loop), "desk-mixer", p);
    pa_proplist_free(p);
    pa_context_set_state_callback(ctx, on_state, NULL);
    pa_context_connect(ctx, NULL, PA_CONTEXT_NOFAIL, NULL);
    return G_SOURCE_REMOVE;
}

void mixer_init(void) {
    if (!load_pos(&mix)) mix.mtop = wth.mtop + 160 + GRID;          /* під погодою (по горизонталі — центр) */
    card_init(&mix, "desktop-mixer", "mixer");
    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    master.idx = PA_INVALID_INDEX;
    row_build(&master);
    add_class(master.row, "m-master");
    GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(sep, "m-sep");
    list = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(cols), master.row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cols), sep, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cols), list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mix.card), cols, FALSE, FALSE, 0);
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(mix.win)));
    rows = g_hash_table_new(NULL, NULL);
    loop = pa_glib_mainloop_new(NULL);
    connect_later(NULL);
}
