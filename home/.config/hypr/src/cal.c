/* КАЛЕНДАР — див. desk.c */
#include "desk.h"

Card cal = { .pos_name = "deskcal.pos", .off_name = "calendar.off", .mright = CAL_RIGHT, .mtop = CAL_TOP };

static const char *MONTHS[] = { "Січень", "Лютий", "Березень", "Квітень", "Травень", "Червень",
                                "Липень", "Серпень", "Вересень", "Жовтень", "Листопад", "Грудень" };
static const char *WEEKDAYS[] = { "Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Нд" };

/* ═════════════ КАЛЕНДАР ═════════════ */
static int cal_day = -1;

static void cal_build(void) {
    GDateTime *now = g_date_time_new_now_local();
    int y = g_date_time_get_year(now), m = g_date_time_get_month(now), today = g_date_time_get_day_of_month(now);
    g_date_time_unref(now);
    cal_day = today;

    GList *kids = gtk_container_get_children(GTK_CONTAINER(cal.card));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(kids);

    char *t = g_strdup_printf("%s %d", MONTHS[m - 1], y);
    GtkWidget *title = gtk_label_new(t);
    g_free(t);
    add_class(title, "month");
    gtk_box_pack_start(GTK_BOX(cal.card), title, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    for (int c = 0; c < 7; c++) {
        GtkWidget *l = gtk_label_new(WEEKDAYS[c]);
        add_class(l, "wd");
        if (c >= 5) add_class(l, "weekend");
        gtk_grid_attach(GTK_GRID(grid), l, c, 0, 1, 1);
    }
    GDateTime *first = g_date_time_new_local(y, m, 1, 0, 0, 0);
    int col = g_date_time_get_day_of_week(first) - 1;          /* 1 = понеділок → колонка 0 */
    g_date_time_unref(first);
    int row = 1;
    for (int d = 1, days = g_date_get_days_in_month(m, y); d <= days; d++) {
        char num[4];
        g_snprintf(num, sizeof num, "%d", d);
        GtkWidget *l = gtk_label_new(num);
        add_class(l, "day");
        if (col >= 5) add_class(l, "weekend");
        if (d < today) add_class(l, "past");
        if (d == today) add_class(l, "today");
        gtk_grid_attach(GTK_GRID(grid), l, col, row, 1, 1);
        if (++col == 7) { col = 0; row++; }
    }
    for (int r = (col ? row + 1 : row); r <= 6; r++) {   /* завжди 6 тижнів — висота календаря не скаче */
        GtkWidget *l = gtk_label_new("");
        add_class(l, "day");
        gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(cal.card), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(cal.card);
}

static gboolean cal_tick(gpointer d) {             /* раз на хвилину: чи не настав новий день (і після сну) */
    GDateTime *now = g_date_time_new_now_local();
    if (g_date_time_get_day_of_month(now) != cal_day) cal_build();
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

/* «Система» — рівно з календарем по висоті */
static void on_cal_alloc(GtkWidget *w, GdkRectangle *a, gpointer d) {
    if (!sysm.card || a->height < 50) return;
    int cur = -1;
    gtk_widget_get_size_request(sysm.card, NULL, &cur);
    if (cur != a->height) gtk_widget_set_size_request(sysm.card, SYS_W, a->height);
}

void cal_init(void) {
    card_init(&cal, "desktop-calendar", "cal");
    g_signal_connect(cal.card, "size-allocate", G_CALLBACK(on_cal_alloc), NULL);
    cal_build();
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(cal.win)));
    g_timeout_add_seconds(60, cal_tick, NULL);
}
