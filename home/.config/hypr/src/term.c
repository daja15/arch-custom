/* ТЕРМІНАЛ НА СТОЛІ — картка з живим терміналом (zsh), як решта віджетів (див. desk.c)
 *
 *   • вигляд — як у карток: напівпрозорий фон, рамка, заокруглення; кольори 16 ANSI —
 *     з теми (~/.config/theme/gen/kitty.conf), шрифт — як у kitty;
 *   • перетягується за заголовок / край картки по сітці 20 px (~/.cache/deskterm.pos);
 *   • клік по терміналу — клавіатура переходить сюди; клік по вікну — назад;
 *   • оболонка завершилась (exit / Ctrl+D) — одразу запускається нова;
 *   • шар «фон»: під усіма вікнами, як календар.
 *
 *   Клавіші: Ctrl+Shift+C / V — копіювати / вставити;
 *            Ctrl+= / Ctrl+- / Ctrl+0 — більший / менший / звичайний шрифт;
 *            Ctrl+Shift+стрілки — ширше / вужче / вище / нижче (2 стовпці / 1 рядок);
 *            Ctrl+клік по посиланню — відкрити.
 *   Розмір і масштаб пам'ятаються: ~/.cache/deskterm.size («стовпці рядки масштаб»).
 *   Економія: курсор не блимає (блимання = перемальовка двічі на секунду), звук дзвінка вимкнено.
 *   Потрібен пакет vte3 (без нього desk збирається без цієї картки — build.sh). */
#include "desk.h"
#include <stdlib.h>
#include <vte/vte.h>

#define TERM_COLS  80          /* розмір за замовчуванням */
#define TERM_ROWS  20

Card trm = { .pos_name = "deskterm.pos", .off_name = "terminal.off", .mright = 1100, .mtop = CAL_TOP };

static VteTerminal *vt;
static GtkWidget *title;
static int cols = TERM_COLS, rows = TERM_ROWS;
static double scale = 1.0;

/* ── розмір: VTE просить мінімум 1 клітинку, а layer-shell бере МІНІМАЛЬНИЙ розмір → задаємо явно ── */
static void size_save(void) {
    char *f = g_build_filename(g_get_user_cache_dir(), "deskterm.size", NULL), num[G_ASCII_DTOSTR_BUF_SIZE];
    char *s = g_strdup_printf("%d %d %s\n", cols, rows, g_ascii_formatd(num, sizeof num, "%.2f", scale));  /* крапка, не кома (uk_UA) */
    g_file_set_contents(f, s, -1, NULL);
    g_free(s); g_free(f);
}
static void size_load(void) {
    char *f = g_build_filename(g_get_user_cache_dir(), "deskterm.size", NULL), *s = NULL;
    if (g_file_get_contents(f, &s, NULL, NULL)) {
        char **p = g_strsplit_set(g_strstrip(s), " ", 3);
        if (p[0] && p[1]) { cols = CLAMP(atoi(p[0]), 20, 300); rows = CLAMP(atoi(p[1]), 4, 100); }
        if (p[0] && p[1] && p[2]) scale = CLAMP(g_ascii_strtod(p[2], NULL), 0.5, 3.0);
        g_strfreev(p);
    }
    g_free(s); g_free(f);
}
static void size_apply(void) {
    vte_terminal_set_font_scale(vt, scale);
    long cw = vte_terminal_get_char_width(vt), ch = vte_terminal_get_char_height(vt);
    GtkBorder pad = { 0 };
    gtk_style_context_get_padding(gtk_widget_get_style_context(GTK_WIDGET(vt)),
                                  gtk_widget_get_state_flags(GTK_WIDGET(vt)), &pad);
    gtk_widget_set_size_request(GTK_WIDGET(vt), cols * cw + pad.left + pad.right, rows * ch + pad.top + pad.bottom);
    vte_terminal_set_size(vt, cols, rows);
    if (trm.win) gtk_window_resize(GTK_WINDOW(trm.win), 1, 1);    /* стиснути вікно до нового розміру */
}
static void on_char_size(VteTerminal *t, guint w, guint h, gpointer d) { size_apply(); }

/* ── кольори й шрифт: з теми (той самий файл, що й для kitty) ── */
static void theme_load(void) {
    GdkRGBA pal[16], fg, cur, sel, bg = { 0, 0, 0, 0 };            /* фон прозорий — видно картку */
    gboolean have[16] = { 0 }, hfg = FALSE, hcur = FALSE, hsel = FALSE;
    char *font = NULL;
    char *f = g_build_filename(g_get_home_dir(), ".config/theme/gen/kitty.conf", NULL), *s = NULL;
    if (g_file_get_contents(f, &s, NULL, NULL)) {
        char **lines = g_strsplit(s, "\n", -1);
        for (char **l = lines; *l; l++) {
            char key[32], val[96];
            if (sscanf(*l, "%31s %95[^\n]", key, val) != 2 || key[0] == '#') continue;
            g_strstrip(val);
            int n;
            if (sscanf(key, "color%d", &n) == 1 && n >= 0 && n < 16) have[n] = gdk_rgba_parse(&pal[n], val);
            else if (!strcmp(key, "foreground"))           hfg  = gdk_rgba_parse(&fg, val);
            else if (!strcmp(key, "cursor"))               hcur = gdk_rgba_parse(&cur, val);
            else if (!strcmp(key, "selection_background")) hsel = gdk_rgba_parse(&sel, val);
            else if (!strcmp(key, "font_family"))          { g_free(font); font = g_strdup(val); }
        }
        g_strfreev(lines);
    }
    g_free(s); g_free(f);
    gboolean all = TRUE;
    for (int i = 0; i < 16; i++) all &= have[i];
    if (!hfg) gdk_rgba_parse(&fg, TH_TEXT);
    vte_terminal_set_colors(vt, &fg, &bg, all ? pal : NULL, all ? 16 : 0);
    if (!hcur) gdk_rgba_parse(&cur, TH_ACCENT);
    vte_terminal_set_color_cursor(vt, &cur);
    GdkRGBA cfg; gdk_rgba_parse(&cfg, TH_BG);
    vte_terminal_set_color_cursor_foreground(vt, &cfg);
    if (hsel) vte_terminal_set_color_highlight(vt, &sel);

    char *desc = g_strdup_printf("%s %s", font ? font : TH_FONT_UI, TH_FONT_SIZE);
    PangoFontDescription *fd = pango_font_description_from_string(desc);
    vte_terminal_set_font(vt, fd);
    pango_font_description_free(fd);
    g_free(desc); g_free(font);
}

/* ── оболонка ── */
static void on_spawned(VteTerminal *t, GPid pid, GError *err, gpointer d) {
    if (err) g_printerr("deskterm: %s\n", err->message);
}
static void spawn_shell(void) {
    const char *sh = g_getenv("SHELL");
    char *argv[] = { (char *)(sh && *sh ? sh : "/bin/zsh"), NULL };
    char **env = g_get_environ();
    env = g_environ_setenv(env, "NO_FETCH", "1", TRUE);           /* без логотипа neofetch (~/.zshrc) */
    env = g_environ_setenv(env, "TERM", "xterm-256color", TRUE);
    env = g_environ_setenv(env, "COLORTERM", "truecolor", TRUE);
    env = g_environ_unsetenv(env, "KITTY_WINDOW_ID");             /* desk могли запустити з kitty */
    env = g_environ_unsetenv(env, "KITTY_PID");
    vte_terminal_spawn_async(vt, VTE_PTY_DEFAULT, g_get_home_dir(), argv, env, G_SPAWN_SEARCH_PATH,
                             NULL, NULL, NULL, -1, NULL, on_spawned, NULL);
    g_strfreev(env);
}
static gboolean respawn(gpointer d) {
    vte_terminal_reset(vt, TRUE, TRUE);
    spawn_shell();
    return G_SOURCE_REMOVE;
}
static void on_exited(VteTerminal *t, int status, gpointer d) {
    g_timeout_add(300, respawn, NULL);                            /* пауза — щоб не крутитись, якщо оболонка падає одразу */
}

/* ── заголовок: те, що оболонка пише в заголовок вікна (oh-my-zsh: «користувач@хост: тека») ── */
static void on_termprop(VteTerminal *t, const char *prop, gpointer d) {
    if (strcmp(prop, VTE_TERMPROP_XTERM_TITLE)) return;
    const char *s = vte_terminal_get_termprop_string(vt, VTE_TERMPROP_XTERM_TITLE, NULL);
    gtk_label_set_text(GTK_LABEL(title), s && *s ? s : "Термінал");
}

/* ── мишка: фокус після кліку; Ctrl+клік — відкрити посилання ── */
static gboolean on_click(GtkWidget *w, GdkEventButton *e, gpointer d) {
    if (e->type == GDK_BUTTON_PRESS && e->button == 1 && (e->state & GDK_CONTROL_MASK)) {
        char *uri = vte_terminal_hyperlink_check_event(vt, (GdkEvent *)e);
        if (!uri) uri = vte_terminal_match_check_event(vt, (GdkEvent *)e, NULL);
        if (uri) {
            gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, NULL);
            g_free(uri);
            return TRUE;
        }
    }
    gtk_widget_grab_focus(w);
    return FALSE;                                                 /* далі — виділення тексту самим VTE */
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    guint m = e->state & gtk_accelerator_get_default_mod_mask();
    guint k = gdk_keyval_to_lower(e->keyval);
    if (m == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        switch (k) {
            case GDK_KEY_c: vte_terminal_copy_clipboard_format(vt, VTE_FORMAT_TEXT); return TRUE;
            case GDK_KEY_v: vte_terminal_paste_clipboard(vt); return TRUE;
            case GDK_KEY_Left:  cols = MAX(20, cols - 2);  break;
            case GDK_KEY_Right: cols = MIN(300, cols + 2); break;
            case GDK_KEY_Up:    rows = MAX(4, rows - 1);   break;
            case GDK_KEY_Down:  rows = MIN(100, rows + 1); break;
            default: return FALSE;
        }
        size_apply(); size_save();
        return TRUE;
    }
    if (m == GDK_CONTROL_MASK) {
        switch (k) {
            case GDK_KEY_equal: case GDK_KEY_plus: case GDK_KEY_KP_Add: scale = MIN(3.0, scale + 0.1); break;
            case GDK_KEY_minus: case GDK_KEY_KP_Subtract:               scale = MAX(0.5, scale - 0.1); break;
            case GDK_KEY_0: case GDK_KEY_KP_0:                          scale = 1.0; break;
            default: return FALSE;
        }
        size_apply(); size_save();
        return TRUE;
    }
    return FALSE;
}

void term_init(void) {
    load_pos(&trm);
    size_load();
    card_init(&trm, "desktop-terminal", "term");
    /* клавіатура: віддається, коли клікнули по картці (інакше все йде у вікна) */
    gtk_layer_set_keyboard_mode(GTK_WINDOW(trm.win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    add_class(head, "t-head");
    GtkWidget *icon = gtk_label_new("\xef\x80\xa0");              /* nf-fa-terminal (U+F120) */
    add_class(icon, "t-icon");
    title = gtk_label_new("Термінал");
    add_class(title, "t-title");
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_START);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(head), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), title, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(trm.card), head, FALSE, FALSE, 0);

    vt = VTE_TERMINAL(vte_terminal_new());
    vte_terminal_set_scrollback_lines(vt, 5000);
    vte_terminal_set_cursor_blink_mode(vt, VTE_CURSOR_BLINK_OFF);
    vte_terminal_set_audible_bell(vt, FALSE);
    vte_terminal_set_mouse_autohide(vt, TRUE);
    vte_terminal_set_allow_hyperlink(vt, TRUE);
    vte_terminal_set_bold_is_bright(vt, FALSE);
    vte_terminal_set_scroll_on_keystroke(vt, TRUE);
    vte_terminal_set_scroll_on_output(vt, FALSE);
    VteRegex *re = vte_regex_new_for_match("(https?|ftp|file)://[^\\s<>\"'()]+[^\\s<>\"'().,;:!?]",
                                           -1, 0x00000400u /* PCRE2_MULTILINE */, NULL);
    if (re) {
        int tag = vte_terminal_match_add_regex(vt, re, 0);
        vte_terminal_match_set_cursor_name(vt, tag, "pointer");
        vte_regex_unref(re);
    }
    theme_load();
    g_signal_connect(vt, "char-size-changed", G_CALLBACK(on_char_size), NULL);
    g_signal_connect(vt, "child-exited", G_CALLBACK(on_exited), NULL);
    g_signal_connect(vt, "termprop-changed", G_CALLBACK(on_termprop), NULL);
    g_signal_connect(vt, "button-press-event", G_CALLBACK(on_click), NULL);
    g_signal_connect(vt, "key-press-event", G_CALLBACK(on_key), NULL);
    gtk_box_pack_start(GTK_BOX(trm.card), GTK_WIDGET(vt), TRUE, TRUE, 0);
    size_apply();
    spawn_shell();
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(trm.win)));
}
