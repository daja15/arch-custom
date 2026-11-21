/* ╭──────────────────────────────────────────────────────────────╮
 * │  ISLANDS — кліки waybar лише по острівцях                    │
 * │  (GTK3-модуль, вантажиться всередину waybar через GTK_MODULES)│
 * │                                                              │
 * │  Waybar — прозора смуга на всю ширину (або висоту) екрана,   │
 * │  видно лише острівці. Але кліки смуга забирає ВСЮ, тож між   │
 * │  острівцями вікна під нею не клацались. Модуль обмежує зону  │
 * │  кліків (input region) видимими острівцями:                  │
 * │    • віджет з фоном або рамкою — острівець цілком;           │
 * │    • прозорий контейнер — дивимось усередину;                │
 * │    • прозорий кінцевий віджет (напис, іконка) — сам по собі. │
 * │  Схована панель (.hidden / .mode-invisible) — без кліків.    │
 * │                                                              │
 * │  Запуск: GTK_MODULES=~/.local/lib/waybar-islands.so waybar   │
 * │  Збірка — ~/.config/hypr/src/build.sh islands                │
 * ╰──────────────────────────────────────────────────────────────╯ */
#include <gtk/gtk.h>
#include <stdlib.h>

/* Чи має віджет видимий фон або рамку (тобто це «острівець») */
static gboolean has_body(GtkWidget *w) {
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    GtkStateFlags st = gtk_style_context_get_state(ctx);
    GdkRGBA *bg = NULL;
    gtk_style_context_get(ctx, st, "background-color", &bg, NULL);
    gboolean body = bg && bg->alpha > 0.01;
    if (bg) gdk_rgba_free(bg);
    if (!body) {
        GtkBorder b;
        gtk_style_context_get_border(ctx, st, &b);
        GdkRGBA *bc = NULL;
        gtk_style_context_get(ctx, st, "border-top-color", &bc, NULL);
        body = (b.top || b.bottom || b.left || b.right) && bc && bc->alpha > 0.01;
        if (bc) gdk_rgba_free(bc);
    }
    return body;
}

/* Додати прямокутник віджета (без CSS-відступів margin) у координатах вікна */
static void add_rect(GtkWidget *w, GtkWidget *top, cairo_region_t *r) {
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    if (a.width <= 1 || a.height <= 1) return;
    int x = 0, y = 0;
    if (!gtk_widget_translate_coordinates(w, top, 0, 0, &x, &y)) return;
    GtkBorder m;
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    gtk_style_context_get_margin(ctx, gtk_style_context_get_state(ctx), &m);
    cairo_rectangle_int_t rc = { x + m.left, y + m.top,
                                 a.width - m.left - m.right, a.height - m.top - m.bottom };
    if (rc.width > 0 && rc.height > 0) cairo_region_union_rectangle(r, &rc);
}

static void collect(GtkWidget *w, gpointer data);

typedef struct { GtkWidget *top; cairo_region_t *r; } Walk;

static void collect(GtkWidget *w, gpointer data) {
    Walk *k = data;
    if (!gtk_widget_get_visible(w) || !gtk_widget_get_mapped(w)) return;
    if (has_body(w) || !GTK_IS_CONTAINER(w)) { add_rect(w, k->top, k->r); return; }
    gtk_container_forall(GTK_CONTAINER(w), collect, k);
}

/* Перерахувати зону кліків; застосувати, лише якщо вона змінилась */
static void update(GtkWidget *win) {
    GdkWindow *gw = gtk_widget_get_window(win);
    if (!gw) return;
    GtkStyleContext *ctx = gtk_widget_get_style_context(win);
    cairo_region_t *r = cairo_region_create();
    gboolean hidden = gtk_style_context_has_class(ctx, "hidden")
                   || gtk_style_context_has_class(ctx, "mode-invisible");
    if (!hidden) {
        Walk k = { win, r };
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(win));
        if (child) collect(child, &k);
    }
    cairo_region_t *old = g_object_get_data(G_OBJECT(win), "islands-region");
    if (old && cairo_region_equal(old, r)) { cairo_region_destroy(r); return; }
    gdk_window_input_shape_combine_region(gw, r, 0, 0);
    g_object_set_data_full(G_OBJECT(win), "islands-region", r, (GDestroyNotify)cairo_region_destroy);
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer d) { update(w); return FALSE; }
static void on_alloc(GtkWidget *w, GdkRectangle *a, gpointer d) { update(w); }
static void on_style(GtkWidget *w, gpointer d) {
    g_object_set_data(G_OBJECT(w), "islands-region", NULL);   /* скинути — застосувати наново */
    update(w);
}

/* Ловимо появу кожного вікна; беремо лише саму панель (window#waybar) */
static gboolean on_realize(GSignalInvocationHint *h, guint n, const GValue *v, gpointer d) {
    GtkWidget *w = g_value_get_object(&v[0]);
    if (!GTK_IS_WINDOW(w) || gtk_window_get_window_type(GTK_WINDOW(w)) != GTK_WINDOW_TOPLEVEL) return TRUE;
    if (g_strcmp0(gtk_widget_get_name(w), "waybar") != 0) return TRUE;
    if (g_object_get_data(G_OBJECT(w), "islands-hooked")) return TRUE;
    g_object_set_data(G_OBJECT(w), "islands-hooked", GINT_TO_POINTER(1));
    g_signal_connect(w, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect_after(w, "size-allocate", G_CALLBACK(on_alloc), NULL);
    g_signal_connect_after(w, "style-updated", G_CALLBACK(on_style), NULL);
    return TRUE;
}

G_MODULE_EXPORT void gtk_module_init(gint *argc, gchar ***argv) {
    unsetenv("GTK_MODULES");   /* програми, які запускає waybar (по кліку), модуль не успадковують */
    g_signal_add_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), 0, on_realize, NULL, NULL);
}
