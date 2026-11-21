/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  DESK — віджети робочого столу (C + GTK3, layer-shell)       │
 * │                                                              │
 * │  Одна програма — два віджети праворуч (кожен вмикається      │
 * │  окремо, див. ~/.config/hypr/desk.sh):                       │
 * │                                                              │
 * │  КАЛЕНДАР — місяць і рік, дні тижня з понеділка, сьогодні    │
 * │    виділене, минулі дні сіруваті. Перемальовується лише      │
 * │    коли змінився день.                                       │
 * │                                                              │
 * │  МУЗИКА — назва й виконавець, смуга прогресу, час.           │
 * │    Зіграна частина — хвиля, що біжить, поки грає; на паузі   │
 * │    плавно випрямляється й завмирає.                          │
 * │    Плеєр — «активний», як для SUPER+Shift+… (music-ctl):     │
 * │    той, що грає (mpv — «Пошук», kew — «Бібліотека»), інакше  │
 * │    той, що грав останнім. Дані — з D-Bus (MPRIS).            │
 * │                                                              │
 * │  Обидва: перетягуються мишкою по сітці 20 px, позиції —      │
 * │  ~/.cache/deskcal.pos і ~/.cache/deskmusic.pos; шар «фон»    │
 * │  (під усіма вікнами).                                        │
 * │                                                              │
 * │  Економія:                                                   │
 * │   • хвиля малюється НЕ через GTK, а у власній маленькій      │
 * │     поверхні Wayland (subsurface): GTK3 витрачав ~0,8 мс на  │
 * │     кожен кадр лише на себе. Хвиля намальована один раз —    │
 * │     щокадру готова картинка лише зсувається; новий кадр —    │
 * │     лише коли екран показав попередній;                      │
 * │   • хвиля — 24 кадри/с на спільній «сітці кадрів» з          │
 * │     еквалайзером (hyprwatch): малюють в один момент, тож     │
 * │     Hyprland складає екран раз на обидва; лише поки грає й   │
 * │     віджет видно;                                            │
 * │     закритий НЕпрозорим вікном (Firefox…) → 0 навантаження;  │
 * │     крізь прозорі вікна (kitty) видно — працює. Події        │
 * │     Hyprland (socket2), без опитування;                      │
 * │   • позиція з плеєра — раз на секунду, між запитами          │
 * │     рахується сама (плавно).                                 │
 * │                                                              │
 * │  ГАРЯЧІ КРАЇ (невидимі смужки, лише події — 0 навантаження): │
 * │    вгорі — курсор торкнувся → waybar (WaybarHover);          │
 * │    зліва посередині — затримав курсор → «Музика» (MusicShow);│
 * │    справа посередині — висувається картка «Зв'язок»;         │
 * │    поки панель «Зв'язок» висунута — «ловець кліків»: клік    │
 * │    повз неї (крім чату, VLC, меню) ховає панель.             │
 * │                                                              │
 * │  Сигнали: SIGHUP — перечитати, які віджети показувати        │
 * │  (desk.sh); SIGUSR1 / SIGUSR2 — панель «Зв'язок» висунута /  │
 * │  схована (hypr/comms.lua). Програма працює завжди (краї).    │
 * │  Кольори, шрифт, заокруглення — з теми (theme.conf).         │
 * │  Збірка — ~/.config/hypr/src/build.sh                        │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#include "desk.h"

GDBusConnection *bus;

/* Вигляд карток. Кольори (@bg, @accent…) і шрифт — з теми: gen/theme.css підключається в main() */
static const char *CSS =
    /* Шрифти, кнопки, перемикачі, рядки списків, шкали — спільний UI-кіт з теми (gen/theme.css:
     * .ui-card, .t-title/.t-subtitle/.t-body/.t-caption, .ui-btn, .ui-icon-btn, .ui-row, switch…).
     * Класи кіту віджети отримують автоматично — див. KIT у add_class(). Тут — лише розкладка
     * й те, чим картки відрізняються одна від одної. */
    "window { background: none; }\n"
    ".o-check { font-size: 12pt; }\n"
    ".cal, .time, .s-val, .b-pct, .d-sub, .pad-lat, .n-count, .w-temp { font-family: \"" TH_FONT_MONO "\"; }\n"   /* числа — рівними стовпцями */
    /* календар */
    ".month { margin-bottom: 12px; }\n"
    ".wd    { margin-bottom: 4px; }\n"
    ".wd.weekend { color: @red; }\n"
    ".day   { min-width: 32px; min-height: 28px; border-radius: " G_STRINGIFY(TH_RADIUS_CONTROL) "px; font-size: 11pt; }\n"
    ".day.weekend { color: @red-soft; }\n"
    ".day.past    { color: @dim; }\n"
    ".day.today   { background-color: @accent; color: @bg; font-weight: bold; }\n"
    /* музика */
    ".music .t-title { color: @text; }\n"                         /* назва пісні — головне, не акцентом */
    ".artist { color: @accent2; margin-top: 2px; margin-bottom: 12px; }\n"
    ".idle .t-title { color: @muted; font-weight: normal; }\n"
    ".time   { margin-top: 4px; }\n"
    /* картка «Зв'язок» (правий край) — та сама картка, але суцільна й притиснута до краю */
    ".comms  { background-color: @surface; border-right: none; padding: 10px 8px;"
    "          border-radius: " G_STRINGIFY(TH_RADIUS) "px 0 0 " G_STRINGIFY(TH_RADIUS) "px; }\n"
    ".comms .head { margin: 0 8px 6px 8px; }\n"
    ".comms button { background: transparent; border: none; border-radius: " G_STRINGIFY(TH_RADIUS_CONTROL) "px; padding: 6px 10px; box-shadow: none; }\n"
    ".comms button:hover { background: alpha(@overlay, 0.8); }\n"
    ".comms .icon { font-size: 14pt; margin-right: 8px; }\n"
    ".comms .name { font-weight: bold; }\n"
    ".comms .add .icon { color: @accent2; }\n"
    /* погода */
    ".w-icon  { font-family: \"" TH_FONT_MONO "\"; font-size: 30pt; }\n"
    ".w-desc  { color: @accent2; margin-top: 4px; }\n"
    ".w-feel  { color: @muted; margin-top: 2px; }\n"
    ".w-place { margin-top: 12px; }\n"
    ".w-upd   { margin-top: 2px; }\n"
    ".w-sep   { background-color: @line; min-width: 1px; margin: 0 16px; }\n"   /* лінія між «зараз» і «день» */
    ".w-dayhead { margin-bottom: 2px; }\n"
    ".w-name  { color: @muted; }\n"
    ".w-val   { font-family: \"" TH_FONT_MONO "\"; }\n"
    /* мікшер — як був: компактний, ширина за вмістом (стовпчики), росте від свого центру */
    ".ui-card.mixer { padding: 10px 12px; font-size: 9pt; font-family: \"" TH_FONT_MONO "\"; }\n"
    ".ui-card.mixer > * { margin-left: 0; margin-right: 0; }\n"
    ".m-col   { min-width: 52px; }\n"
    ".m-name  { color: @muted; font-size: 8pt; }\n"
    ".m-master .m-name { color: @accent2; font-weight: bold; }\n"
    ".m-sep   { background-color: @line; min-width: 1px; margin: 4px 6px; }\n"
    ".m-mute  { background: none; border: none; box-shadow: none; color: @muted; padding: 0; min-height: 0; font-size: 12pt; }\n"
    ".m-mute:checked { color: @red; }\n"
    /* сповіщення */
    ".notify   { padding-right: 12px; }\n"
    ".n-count  { color: @bg; background-color: @accent; border-radius: 8px; padding: 0 6px; font-size: 8pt; }\n"
    ".n-clear:hover { color: @red; }\n"
    ".n-bell   { background: none; border: none; box-shadow: none; padding: 0; min-height: 0; min-width: 24px; }\n"   /* дзвіночок — кнопка «не турбувати» */
    ".n-bell:hover { color: @accent2; }\n"
    ".ui-head .n-bell.dnd { color: @red; }\n"
    ".n-dnd    { color: @red-text; }\n"
    ".n-empty  { margin-top: 12px; }\n"
    ".n-row    { background-color: alpha(@surface, 0.8); border-radius: " G_STRINGIFY(TH_RADIUS) "px; padding: 8px 12px; margin: 8px 4px 0 0; }\n"
    ".n-summary { font-weight: bold; }\n"
    ".n-body   { color: @muted; }\n"
    /* геймпади */
    ".pad-conn { color: @accent; font-size: 12pt; }\n"
    ".pad-name { margin-bottom: 8px; }\n"
    ".pad-sep  { background-color: @line; min-height: 1px; margin: 12px 0; }\n"
    ".pad-off  { margin-top: 8px; }\n"
    ".pad-off:hover { background: @red-bg; color: @red-text; border-color: @red; }\n"
    ".pad-backs { margin-top: 6px; }\n"
    ".tb-row   { margin-top: 4px; margin-bottom: 4px; }\n"
    ".bt-btn.danger { background: @red-bg; color: @red-text; border-color: @red; }\n"   /* «забути» — після першого кліку */                     /* планшет: рядки налаштувань */
    ".tb-press mark label { font-size: 8pt; color: @dim; }\n"
    ".tb-outs  { margin-bottom: 4px; }\n"
    ".tb-map   { font-size: 9pt; }\n"
    ".dc-row .ui-btn { min-height: 26px; padding: 0 10px; }\n"
    ".bl-box { margin-top: 4px; }\n"
    ".tb-outs .active { background: @accent-bg; color: @accent; border-color: @accent; }\n"
    /* заряд */
    ".b-head   { margin-bottom: 12px; }\n"
    ".b-icon   { font-size: 16pt; min-width: 26px; }\n"
    ".b-bar block.filled { background-color: @green; }\n"
    ".b-row.low .b-bar block.filled { background-color: @red; }\n"
    ".b-row.low .b-pct { color: @red; }\n"
    ".b-row.charging .b-pct { color: @yellow; }\n"
    /* Bluetooth */
    ".ui-head .bt-logo { font-size: 14pt; }\n"   /* знак Bluetooth високий — менший, ніж інші значки */
    ".bt-bar   { margin-bottom: 4px; }\n"
    ".bt-state.busy { color: @accent; }\n"
    ".bt-status { color: @red-text; margin-top: 8px; }\n"
    ".bt-row:first-child { margin-top: 8px; }\n"
    ".bt-icon  { color: @muted; font-size: 15pt; min-width: 24px; }\n"
    ".bt-row.conn .bt-icon { color: @accent; }\n"
    ".bt-row.conn .bt-sub { color: @accent; }\n"
    ".bt-none  { margin-top: 12px; }\n"
    ".s-bar    { background-color: @line; border-radius: 1px; }\n"
    ".s-bar.on { background-color: @green; }\n"
    ".bt-btn   { font-size: 12pt; padding: 0 3px; }\n"
    /* термінал */
    ".term     { padding: 10px 14px 12px 14px; }\n"
    ".t-head   { margin-bottom: 8px; }\n"
    "vte-terminal { padding: 0; background-color: transparent; }\n"
    /* система */
    ".s-row    { margin-top: 4px; }\n"
    ".s-icon   { min-width: 18px; }\n"
    ".s-name   { color: @muted; }\n"
    ".s-row.warn .s-val { color: @yellow; }\n"
    ".s-row.crit .s-val { color: @red; }\n"
    ".s-row.warn levelbar block.filled { background-color: @yellow; }\n"
    ".s-row.crit levelbar block.filled { background-color: @red; }\n"
    ".s-power  { margin-top: 4px; }\n"
    /* Dev-стек */
    ".d-logo   { margin-right: 4px; color: @blue; font-size: 14pt; }\n"
    ".d-proj   { margin-bottom: 8px; }\n"
    ".d-state  { border-radius: " G_STRINGIFY(TH_RADIUS_CONTROL) "px; padding: 0 8px; }\n"
    ".d-state.on   { background-color: @accent-bg; color: @accent; }\n"
    ".d-state.off  { background-color: alpha(@surface, 0.8); color: @muted; }\n"
    ".d-state.busy { background-color: alpha(@yellow, 0.15); color: @yellow; }\n"
    ".ui-row .d-kind { color: @faint; }\n"                         /* сервіс вимкнено — значок блідий */
    ".ui-row.up .d-kind { color: @green; }\n"                       /* працює — зелений */
    ".ui-row.up .d-sub { color: @green; }\n"
    ".ui-row.up:hover .d-sub { color: @accent; }\n"
    /* висувні панелі: [Сповіщення][панель][язички] — панель зліва пристикована (без заокруглення),
     * язичок-кружок — праворуч від неї. Стан на язичку: .on — працює (зелений), .off — вимкнено (блідий),
     * .open — панель висунута (акцентне тло) */
    ".dr-panel.ui-card { border-left: none; border-radius: 0 " G_STRINGIFY(TH_RADIUS_CARD) "px " G_STRINGIFY(TH_RADIUS_CARD) "px 0; }\n"
    ".ui-card.notify.dock-top { border-top-right-radius: 0; }\n"
    ".ui-card.notify.dock-bot { border-bottom-right-radius: 0; }\n"
    ".dr-tab   { background-color: alpha(@bg, 0.72); background-image: none; border: 1px solid @line; border-radius: 50%;"
    "            box-shadow: none; padding: 0; min-width: 36px; min-height: 36px; margin-left: 6px;"   /* кружок, трохи відступає від краю */
    "            color: @muted; font-family: \"" TH_FONT_MONO "\"; font-size: 14pt; }\n"
    ".dr-tab:hover { color: @accent; }\n"
    ".dr-tab.on  { color: @green; }\n"
    ".dr-tab.off { color: @dim; }\n"
    ".dr-tab.open { background-color: @accent-bg; border-color: @accent; }\n"
    ".dr-tab.open:not(.on):not(.off) { color: @accent; }\n"
    ".dr-tabs-top { margin-top: 16px; }\n"
    ".dr-tabs-bot { margin-bottom: 16px; }\n"
    /* Wi-Fi */
    ".wf-lock  { color: @faint; font-family: \"" TH_FONT_MONO "\"; font-size: 11pt; }\n"
    ".ui-row.conn .ui-lead, .ui-row.conn .wf-sub { color: @accent; }\n"
    ".wf-status { color: @red-text; margin-top: 8px; }\n"
    ".wf-pass  { margin-top: 4px; }\n"
    /* ігри */
    ".g-logo   { margin-right: 4px; font-size: 14pt; }\n"
    ".g-icon   { color: @muted; font-size: 14pt; min-width: 22px; }\n"
    ".ui-row:hover .g-icon { color: @accent; }\n";

/* Класи віджетів → класи спільного UI-кіту (gen/theme.css). add_class() додає обидва:
 * старий — для розкладки й особливостей картки, кітовий — шрифт, колір, вигляд кнопок. */
static const char *KIT[][2] = {
    { "card", "ui-card" },
    /* заголовки карток */
    { "month", "t-title" }, { "b-head", "t-title" }, { "bt-head", "t-title" }, { "s-head", "t-title" },
    { "d-head", "t-title" }, { "g-head", "t-title" }, { "n-head", "t-title" }, { "pad-num", "t-title" },
    { "t-title", "t-title" }, { "title", "t-title" },
    /* великі числа */
    { "w-temp", "t-display" },
    /* підзаголовки, розділи */
    { "artist", "t-subtitle" }, { "g-sec", "t-subtitle" }, { "m-sec", "t-subtitle" }, { "head", "t-subtitle" }, { "wd", "t-subtitle" },
    { "pad-name", "t-subtitle" }, { "w-desc", "t-subtitle" }, { "addtitle", "t-subtitle" },
    /* основний текст */
    { "b-name", "t-body" }, { "bt-name", "t-body" }, { "g-name", "t-body" }, { "d-name", "t-body" },
    { "s-name", "t-body" }, { "s-val", "t-body" }, { "n-summary", "t-body" }, { "name", "t-body" },
    { "w-place", "t-body" }, { "wf-sub", "t-caption" }, { "wf-status", "t-caption" }, { "n-dnd", "t-caption" },
    /* дрібні підписи */
    { "time", "t-caption" }, { "s-sub", "t-caption" }, { "d-sub", "t-caption" }, { "g-sub", "t-caption" },
    { "bt-sub", "t-caption" }, { "n-time", "t-caption" }, { "n-app", "t-caption" }, { "w-upd", "t-caption" },
    { "w-feel", "t-caption" }, { "w-name", "t-caption" }, { "w-val", "t-body" }, { "w-dayhead", "t-subtitle" }, { "pad-lat", "t-caption" }, { "pad-bat", "t-caption" }, { "b-pct", "t-caption" },
    { "bt-none", "t-caption" }, { "n-empty", "t-caption" }, { "sub", "t-caption" }, { "n-body", "t-caption" },
    { "d-proj", "t-caption" }, { "d-state", "t-caption" }, { "bt-status", "t-caption" },
    /* іконки */
    { "s-icon", "t-icon" }, { "d-logo", "t-icon" }, { "g-logo", "t-icon" }, { "t-icon", "t-icon" },
    { "b-icon", "t-icon" }, { "bt-icon", "t-icon" }, { "g-icon", "t-icon" }, { "d-kind", "t-icon" },
    { "d-dot", "t-icon" }, { "icon", "t-icon" }, { "w-icon", "t-icon" },
    /* кнопки */
    { "s-btn", "ui-btn" }, { "d-btn", "ui-btn" }, { "bt-scan", "ui-btn" }, { "pad-off", "ui-btn" },
    { "pad-learn", "ui-btn" }, { "main", "primary" }, { "stop", "danger" },
    { "n-clear", "ui-icon-btn" }, { "bt-btn", "ui-icon-btn" }, { "n-bell", "ui-lead" },
    /* рядки списків, поля */
    { "g-row", "ui-row" }, { "d-row", "ui-row" }, { "bt-row", "ui-row" }, { "bt-entry", "ui-entry" },
    /* смужки */
    { "s-meter", "s-meter" },
    /* Bluetooth: заголовок, стан */
    { "bt-logo", "t-icon" }, { "bt-state", "t-caption" },
    /* розділи, вихід звуку, Wi-Fi */
    { "g-sec", "ui-section" }, { "o-row", "ui-row" }, { "o-check", "t-icon" }, { "wf-row", "ui-row" }, { "wf-pass", "ui-entry" },
};

/* ═════════════ СПІЛЬНЕ: вікно-картка, що перетягується по сітці ═════════════ */
int snap(int v) { return MAX(0, (int)floor((v + GRID / 2.0) / GRID) * GRID); }
void add_class(GtkWidget *w, const char *c) {
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    gtk_style_context_add_class(sc, c);
    for (guint i = 0; i < G_N_ELEMENTS(KIT); i++)              /* + клас спільного UI-кіту */
        if (!strcmp(KIT[i][0], c)) gtk_style_context_add_class(sc, KIT[i][1]);
}

gboolean load_pos(Card *c) {
    char *f = g_build_filename(g_get_user_cache_dir(), c->pos_name, NULL), *s = NULL;
    gboolean ok = g_file_get_contents(f, &s, NULL, NULL) && sscanf(s, "%d %d", &c->mright, &c->mtop) == 2;
    g_free(s); g_free(f);
    return ok;
}
static void save_pos(Card *c) {
    char *f = g_build_filename(g_get_user_cache_dir(), c->pos_name, NULL), *s = g_strdup_printf("%d %d\n", c->mright, c->mtop);
    g_file_set_contents(f, s, -1, NULL);
    g_free(s); g_free(f);
}
static void apply_pos(Card *c) {
    if (!c->centered) gtk_layer_set_margin(GTK_WINDOW(c->win), GTK_LAYER_SHELL_EDGE_RIGHT, c->mright);
    gtk_layer_set_margin(GTK_WINDOW(c->win), GTK_LAYER_SHELL_EDGE_TOP, c->mtop);
    c->shown_top = c->mtop; c->shown_right = c->mright;
}
static gboolean enabled(Card *c) {
    char *f = g_build_filename(g_get_home_dir(), ".config/hypr", c->off_name, NULL);
    gboolean on = !g_file_test(f, G_FILE_TEST_EXISTS);
    g_free(f);
    return on;
}

/* Перетягування: вікно прив'язане до правого й верхнього країв, рух мишки змінює відступи.
 * Координати мишки — відносно вікна в його ПОТОЧНІЙ позиції → де було б вікно без сітки,
 * і до найближчої клітинки. Мала зміна — вікно лишається на місці. */
static gboolean on_press(GtkWidget *w, GdkEventButton *e, Card *c) {
    if (e->button != 1) return FALSE;
    c->dragging = TRUE; c->press_x = e->x; c->press_y = e->y;
    c->mright = c->shown_right; c->mtop = c->shown_top;   /* тягнемо звідти, де картку видно */
    return TRUE;
}
static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, Card *c) {
    if (!c->dragging) return FALSE;
    int r = c->centered ? c->mright : snap(c->mright - (int)(e->x - c->press_x)), t = snap(c->mtop + (int)(e->y - c->press_y));
    if (r != c->mright || t != c->mtop) { c->mright = r; c->mtop = t; apply_pos(c); if (c == &ntf) cards_relayout(); }   /* шторка — слідом */
    return TRUE;
}
static gboolean on_release(GtkWidget *w, GdkEventButton *e, Card *c) {
    if (!c->dragging) return FALSE;
    c->dragging = FALSE;
    save_pos(c);
    cards_relayout();
    if (c->moved) c->moved();
    return TRUE;
}

/* Картки виросли (нові рядки) — не налазити одна на одну і не вилазити за низ екрана.
 * Проходимо картки згори вниз: якщо картка перекриває вищу (по горизонталі теж) —
 * опускаємо під неї (+ GRID); не влазить знизу — піднімаємо. Збережені позиції не
 * змінюються: стане менше — картки повернуться на свої місця. */
static Card **all_cards;
static int n_all;
void cards_register(Card *c) { all_cards = g_renew(Card *, all_cards, n_all + 1); all_cards[n_all++] = c; }
/* Картку знищено (геймпад / планшет відключився) — прибрати зі списку, інакше layout_all звернеться до звільненої пам'яті */
void cards_unregister(Card *c) {
    for (int i = 0; i < n_all; i++)
        if (all_cards[i] == c) { memmove(&all_cards[i], &all_cards[i + 1], (n_all - i - 1) * sizeof *all_cards); n_all--; break; }
}
static gint by_top(gconstpointer a, gconstpointer b) {
    Card *x = *(Card *const *)a, *y = *(Card *const *)b;
    if (x == &ntf) return -1;                              /* «Сповіщення» з шторкою — першими: решта розсувається від них */
    if (y == &ntf) return 1;
    return x->mtop - y->mtop;
}
static void card_x(Card *c, int sw, int *x0, int *x1) {
    int w = gtk_widget_get_allocated_width(c->win);
    *x0 = c->centered ? (sw - w) / 2 : sw - c->shown_right - w;
    *x1 = *x0 + w;
}
/* Де стала б картка c у колонці зі зсувом right, якщо вищі (уже розставлені) картки не перекривати */
static int fit_top(GPtrArray *placed, Card *c, int right, int sw, int *x0_out) {
    int h = gtk_widget_get_allocated_height(c->win), w = gtk_widget_get_allocated_width(c->win);
    int x0 = c->centered ? (sw - w) / 2 : sw - right - w, x1 = x0 + w, top = c->mtop;
    gboolean moved = TRUE;
    while (moved) {                                    /* опускати, доки ні з ким не перетинається */
        moved = FALSE;
        for (guint j = 0; j < placed->len; j++) {
            Card *o = placed->pdata[j];
            int ox0, ox1, oh = gtk_widget_get_allocated_height(o->win);
            card_x(o, sw, &ox0, &ox1);
            if (x0 < ox1 + GRID / 2 && ox0 < x1 + GRID / 2 && top < o->shown_top + oh + GRID / 2 && o->shown_top < top + h + GRID / 2) {
                top = o->shown_top + oh + GRID;
                moved = TRUE;
            }
        }
    }
    if (x0_out) *x0_out = x0;
    return top;
}
/* Шторка висувних панелей (див. drawer_card_init нижче): дві групи — верхня (Wi-Fi, Bluetooth) і
 * нижня (Ігри, Dev-стек). Кожна — своє вікно впритул праворуч від «Сповіщень»: [панель][язички].
 * Для розкладки група — ще одна «картка» розміром з те, що видно (язички або панель з язичками):
 * сусідні картки зсуваються, лише коли на них справді налазить. */
typedef struct { GtkWidget *win, *panels, *tabs; Card pc; gboolean bottom; Card *open; } DrGroup;
static DrGroup dr_grp[2];                  /* 0 — верхня, 1 — нижня */
static Card *dr_cards[8];
static int dr_n;
static gboolean dr_any(void) { return dr_grp[0].win || dr_grp[1].win; }
static void drawer_place(int sw, int sh) {
    gboolean nv = gtk_widget_get_visible(ntf.win);
    int x = sw - ntf.shown_right, top = ntf.shown_top;      /* правий край «Сповіщень» */
    int bottom = nv ? top + gtk_widget_get_allocated_height(ntf.win) : sh - 60;
    for (int k = 0; k < 2; k++) {
        DrGroup *g = &dr_grp[k];
        if (!g->win || !gtk_widget_get_visible(g->win)) continue;
        int w = gtk_widget_get_allocated_width(g->win), h = gtk_widget_get_allocated_height(g->win);
        gtk_layer_set_margin(GTK_WINDOW(g->win), GTK_LAYER_SHELL_EDGE_LEFT, x);
        if (g->bottom) gtk_layer_set_margin(GTK_WINDOW(g->win), GTK_LAYER_SHELL_EDGE_BOTTOM, sh - bottom);   /* низ — урівень з низом «Сповіщень» */
        else           gtk_layer_set_margin(GTK_WINDOW(g->win), GTK_LAYER_SHELL_EDGE_TOP, top);
        g->pc.shown_top = g->bottom ? bottom - h : top;
        g->pc.shown_right = sw - x - w;
    }
}
static void drawer_add_placed(GPtrArray *placed) {
    for (int k = 0; k < 2; k++) if (dr_grp[k].win && gtk_widget_get_visible(dr_grp[k].win)) g_ptr_array_add(placed, &dr_grp[k].pc);
}

static guint layout_src;
static gboolean layout_all(gpointer d) {
    layout_src = 0;
    GdkMonitor *mon = gdk_display_get_monitor(gdk_display_get_default(), 0);
    if (!mon) return G_SOURCE_REMOVE;
    GdkRectangle g;
    gdk_monitor_get_geometry(mon, &g);
    GPtrArray *vis = g_ptr_array_new(), *placed = g_ptr_array_new();
    for (int i = 0; i < n_all; i++) if (all_cards[i]->win && gtk_widget_get_visible(all_cards[i]->win)) g_ptr_array_add(vis, all_cards[i]);
    g_ptr_array_sort(vis, by_top);
    gboolean dr = dr_any();
    if (dr && !(vis->len && vis->pdata[0] == &ntf)) { drawer_place(g.width, g.height); drawer_add_placed(placed); }   /* «Сповіщення» сховані */
    for (guint i = 0; i < vis->len; i++) {
        Card *c = vis->pdata[i];
        int h = gtk_widget_get_allocated_height(c->win), w = gtk_widget_get_allocated_width(c->win);
        int right = c->mright, top = c->mtop, x0;
        if (!c->dragging) {
            top = fit_top(placed, c, right, g.width, &x0);
            if (top + h > g.height - GRID && !c->centered) {   /* у своїй колонці не влазить — сусідні колонки */
                for (int k = 1; k <= 8; k++) {
                    int found = 0;
                    for (int sgn = -1; sgn <= 1 && !found; sgn += 2) {   /* спершу ліворуч (до центру), потім праворуч */
                        int r = c->mright - sgn * k * (w + GRID);
                        if (r < 0 || r + w > g.width) continue;
                        int t = fit_top(placed, c, r, g.width, &x0);
                        if (t + h <= g.height - GRID) { right = r; top = t; found = 1; }
                    }
                    if (found) break;
                }
            }
            if (top + h > g.height - GRID) top = MAX(GRID, g.height - h - GRID);
        }
        if (top != c->shown_top || right != c->shown_right) {
            c->shown_top = top; c->shown_right = right;
            gtk_layer_set_margin(GTK_WINDOW(c->win), GTK_LAYER_SHELL_EDGE_TOP, top);
            if (!c->centered) gtk_layer_set_margin(GTK_WINDOW(c->win), GTK_LAYER_SHELL_EDGE_RIGHT, right);
        }
        g_ptr_array_add(placed, c);
        if (c == &ntf && dr) { drawer_place(g.width, g.height); drawer_add_placed(placed); }   /* шторка — одразу за ними */
    }
    g_ptr_array_unref(vis); g_ptr_array_unref(placed);
    return G_SOURCE_REMOVE;
}
void cards_relayout(void) { if (!layout_src) layout_src = g_timeout_add(50, layout_all, NULL); }
static void on_card_alloc(GtkWidget *w, GdkRectangle *a, Card *c) {
    static GHashTable *last;
    if (!last) last = g_hash_table_new(NULL, NULL);
    int key = a->height * 10000 + a->width;
    if (GPOINTER_TO_INT(g_hash_table_lookup(last, c)) == key) return;   /* розмір не змінився */
    g_hash_table_insert(last, c, GINT_TO_POINTER(key));
    cards_relayout();
}

void card_init(Card *c, const char *ns, const char *cls) {
    c->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GdkVisual *vis = gdk_screen_get_rgba_visual(gdk_screen_get_default());
    if (vis) gtk_widget_set_visual(c->win, vis);             /* прозорі заокруглені кути */
    gtk_widget_set_app_paintable(c->win, TRUE);
    gtk_layer_init_for_window(GTK_WINDOW(c->win));
    gtk_layer_set_namespace(GTK_WINDOW(c->win), ns);
    gtk_layer_set_layer(GTK_WINDOW(c->win), g_getenv("DESK_TEST_OVERLAY")    /* для перевірки вигляду: поверх усього */
                        ? GTK_LAYER_SHELL_LAYER_OVERLAY : GTK_LAYER_SHELL_LAYER_BACKGROUND);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(c->win), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(c->win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_anchor(GTK_WINDOW(c->win), GTK_LAYER_SHELL_EDGE_RIGHT, !c->centered);   /* без правого краю — по центру */
    gtk_layer_set_anchor(GTK_WINDOW(c->win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    c->mright = snap(c->mright); c->mtop = snap(c->mtop);
    apply_pos(c);

    GtkWidget *ev = gtk_event_box_new();                      /* ловить мишку для перетягування */
    gtk_widget_add_events(ev, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(ev, "button-press-event", G_CALLBACK(on_press), c);
    g_signal_connect(ev, "motion-notify-event", G_CALLBACK(on_motion), c);
    g_signal_connect(ev, "button-release-event", G_CALLBACK(on_release), c);
    c->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(c->card, "card");
    add_class(c->card, cls);
    if (!c->centered) gtk_widget_set_size_request(c->card, CARD_W, -1);   /* по центру — ширина за вмістом */
    gtk_container_add(GTK_CONTAINER(ev), c->card);
    gtk_container_add(GTK_CONTAINER(c->win), ev);
    c->shown_top = c->mtop; c->shown_right = c->mright;
    g_signal_connect(c->win, "size-allocate", G_CALLBACK(on_card_alloc), c);
    cards_register(c);
}


/* ═════════════ БУДОВА КАРТОК ═════════════ */
GtkWidget *ui_lead(const char *icon) {
    GtkWidget *l = gtk_label_new(icon);
    add_class(l, "ui-lead");
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    return l;
}
GtkWidget *ui_text(const char *primary, const char *secondary, GtkWidget **p_out, GtkWidget **s_out) {
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
    GtkWidget *p = gtk_label_new(primary ? primary : "");
    add_class(p, "t-body");
    gtk_label_set_xalign(GTK_LABEL(p), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(p), 1);            /* ширина — за карткою */
    gtk_box_pack_start(GTK_BOX(col), p, FALSE, FALSE, 0);
    if (p_out) *p_out = p;
    if (secondary) {
        GtkWidget *s = gtk_label_new(secondary);
        add_class(s, "t-caption");
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(s), 1);
        gtk_box_pack_start(GTK_BOX(col), s, FALSE, FALSE, 0);
        if (s_out) *s_out = s;
    }
    return col;
}
GtkWidget *ui_row_box(GtkWidget *lead, GtkWidget *text) {
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    if (lead) gtk_box_pack_start(GTK_BOX(b), lead, FALSE, FALSE, 0);
    if (text) gtk_box_pack_start(GTK_BOX(b), text, TRUE, TRUE, 0);
    return b;
}
GtkWidget *ui_header(Card *c, const char *icon, const char *title, GtkWidget **title_out, GtkWidget **sub_out) {
    GtkWidget *t = NULL, *s = NULL;
    GtkWidget *col = ui_text(title, sub_out ? "" : NULL, &t, &s);
    GtkStyleContext *tc = gtk_widget_get_style_context(t);
    gtk_style_context_remove_class(tc, "t-body");
    gtk_style_context_add_class(tc, "t-title");
    if (s) add_class(s, "ui-sub");
    GtkWidget *head = ui_row_box(ui_lead(icon), col);
    add_class(head, "ui-head");
    gtk_box_pack_start(GTK_BOX(c->card), head, FALSE, FALSE, 0);
    if (title_out) *title_out = t;
    if (sub_out) *sub_out = s;
    return head;
}
GtkWidget *ui_section(GtkWidget *box, const char *title) {
    GtkWidget *l = gtk_label_new(title);
    add_class(l, "t-subtitle");
    add_class(l, "ui-section");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    return l;
}
GtkWidget *ui_actions(GtkWidget *box) {
    GtkWidget *a = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(a), TRUE);
    add_class(a, "ui-actions");
    gtk_box_pack_start(GTK_BOX(box), a, FALSE, FALSE, 0);
    return a;
}

/* Висота списку з прокруткою: до max_rows видимих рядків — без прокрутки, більше — прокрутка */
void ui_list_fit(GtkWidget *scroll, GtkWidget *list, int max_rows) {
    int h = 0, n = 0;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = kids; l; l = l->next) {
        if (!gtk_widget_get_visible(l->data)) continue;
        if (n++ >= max_rows) continue;
        int nat = 0;
        gtk_widget_get_preferred_height(l->data, NULL, &nat);
        h += nat;
    }
    g_list_free(kids);
    if (GTK_IS_BOX(list) && n > 1) h += gtk_box_get_spacing(GTK_BOX(list)) * (MIN(n, max_rows) - 1);
    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(scroll);
    gtk_scrolled_window_set_max_content_height(sw, -1);
    gtk_scrolled_window_set_min_content_height(sw, h);
    gtk_scrolled_window_set_max_content_height(sw, MAX(h, 1));
}

/* ═════════════ ВИСУВНІ ПАНЕЛІ (шторка біля «Сповіщень») ═════════════
 * [Сповіщення][панель][язички]: панель пристикована до правого краю «Сповіщень» (зліва без
 * заокруглення), язички — завжди праворуч від неї (або від «Сповіщень», коли все сховано).
 * Клік по язичку — панель висувається вправо (власна анімація, див. dr_tick), ще клік — ховається.
 * У кожній групі відкрита одна панель. Язички показують стан (увімкнено / вимкнено). */
static DrGroup *dr_group(Card *c) { return &dr_grp[c->win == dr_grp[1].win]; }
/* Висування: ширина «вікна» панелі (GtkScrolledWindow без смуг прокрутки) 0 → CARD_W за DR_MS.
 * Видно праву частину картки — вона виїжджає з-під «Сповіщень». Відкриття — ease-in, закриття — ease-out. */
#define DR_MS 200
typedef struct { double from, to, pos; gint64 t0; guint tick; } DrAnim;
static DrAnim *dr_anim(Card *c) {
    DrAnim *a = g_object_get_data(G_OBJECT(c->rev), "anim");
    if (!a) { a = g_new0(DrAnim, 1); g_object_set_data_full(G_OBJECT(c->rev), "anim", a, g_free); }
    return a;
}
static void dr_width(Card *c, double pos) {
    int w = (int)lround(CARD_W * CLAMP(pos, 0, 1));
    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(c->rev);
    gtk_scrolled_window_set_max_content_width(sw, -1);
    gtk_scrolled_window_set_min_content_width(sw, w);
    gtk_scrolled_window_set_max_content_width(sw, MAX(w, 1));
}
static void dr_stick_right(GtkAdjustment *ha, gpointer d) {   /* завжди показувати праву частину картки */
    double v = gtk_adjustment_get_upper(ha) - gtk_adjustment_get_page_size(ha);
    if (gtk_adjustment_get_value(ha) != v) gtk_adjustment_set_value(ha, v);
}
static void dr_finished(Card *c, gboolean open) {
    if (open) { if (c->shown) c->shown(); }
    else {
        gtk_widget_hide(c->rev);                   /* схована не займає місця */
        gtk_window_resize(GTK_WINDOW(c->win), 1, 1);
        if (c->hidden) c->hidden();
    }
    cards_relayout();
}
static gboolean dr_tick(GtkWidget *w, GdkFrameClock *fc, gpointer d) {
    Card *c = d;
    DrAnim *a = dr_anim(c);
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    if (!a->t0) a->t0 = now;
    double t = MIN(1.0, (now - a->t0) / (DR_MS * 1000.0));
    double e = a->to > a->from ? t * t * t : 1 - pow(1 - t, 3);   /* виїжджає — ease-in, заїжджає — ease-out */
    a->pos = a->from + (a->to - a->from) * e;
    dr_width(c, a->pos);
    if (t < 1) return G_SOURCE_CONTINUE;
    a->tick = 0;
    dr_finished(c, a->to > 0.5);
    return G_SOURCE_REMOVE;
}
static gboolean dr_is_open(Card *c) { return dr_anim(c)->to > 0.5; }
static void dr_set(Card *c, gboolean open, gboolean animate) {
    GtkStyleContext *sc = gtk_widget_get_style_context(c->tab);
    DrGroup *g = dr_group(c);
    DrAnim *a = dr_anim(c);
    if (open) { gtk_widget_show(c->rev); gtk_style_context_add_class(sc, "open"); g->open = c; }
    else { gtk_style_context_remove_class(sc, "open"); if (g->open == c) g->open = NULL; }
    if (ntf.card) {                                /* «Сповіщення»: кут біля відкритої панелі — прямий, щоб прилягала */
        GtkStyleContext *nc = gtk_widget_get_style_context(ntf.card);
        const char *cls = g->bottom ? "dock-bot" : "dock-top";
        if (g->open) gtk_style_context_add_class(nc, cls); else gtk_style_context_remove_class(nc, cls);
    }
    double to = open ? 1 : 0;
    if (a->to == to && (a->tick || a->pos == to)) return;
    if (a->tick) { gtk_widget_remove_tick_callback(c->win, a->tick); a->tick = 0; }
    a->from = a->pos; a->to = to; a->t0 = 0;
    if (animate && gtk_widget_get_mapped(c->win)) a->tick = gtk_widget_add_tick_callback(c->win, dr_tick, c, NULL);
    else { a->pos = to; dr_width(c, to); dr_finished(c, open); }
}
static void on_tab(GtkButton *b, Card *c) {
    DrGroup *g = dr_group(c);
    gboolean open = !dr_is_open(c);
    if (open && g->open && g->open != c) {         /* інша в цій групі відкрита — не перевідкривати, а замінити на місці */
        DrAnim *a = dr_anim(c);
        double p = dr_anim(g->open)->pos;          /* скільки вже висунуто (могла ще їхати) */
        dr_set(g->open, FALSE, FALSE);
        gtk_widget_show(c->rev);
        a->pos = p;
        dr_width(c, p);
        dr_set(c, TRUE, p < 1);                    /* висунута повністю — одразу, ні — доїжджає звідти ж */
        return;
    }
    dr_set(c, open, TRUE);
}
static void dr_group_build(DrGroup *g, gboolean bottom) {
    g->bottom = bottom;
    g->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GdkVisual *vis = gdk_screen_get_rgba_visual(gdk_screen_get_default());
    if (vis) gtk_widget_set_visual(g->win, vis);
    gtk_widget_set_app_paintable(g->win, TRUE);
    gtk_layer_init_for_window(GTK_WINDOW(g->win));
    gtk_layer_set_namespace(GTK_WINDOW(g->win), bottom ? "desktop-drawer-bottom" : "desktop-drawer-top");
    gtk_layer_set_layer(GTK_WINDOW(g->win), g_getenv("DESK_TEST_OVERLAY") ? GTK_LAYER_SHELL_LAYER_OVERLAY : GTK_LAYER_SHELL_LAYER_BACKGROUND);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(g->win), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g->win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_anchor(GTK_WINDOW(g->win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);    /* росте вправо, від «Сповіщень» */
    gtk_layer_set_anchor(GTK_WINDOW(g->win), bottom ? GTK_LAYER_SHELL_EDGE_BOTTOM : GTK_LAYER_SHELL_EDGE_TOP, TRUE);   /* нижня — росте вгору */
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    g->panels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g->tabs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    add_class(g->tabs, bottom ? "dr-tabs-bot" : "dr-tabs-top");
    gtk_widget_set_valign(g->panels, bottom ? GTK_ALIGN_END : GTK_ALIGN_START);
    gtk_widget_set_valign(g->tabs, bottom ? GTK_ALIGN_END : GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(h), g->panels, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), g->tabs, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(g->win), h);
    gtk_widget_show_all(h);
    g->pc.win = g->win;
    g_signal_connect(g->win, "size-allocate", G_CALLBACK(on_card_alloc), &g->pc);
}
/* Значок на язичку: центр за обрисом самого знака (ink), а не за рядком шрифту — у Nerd-значків
 * рядок вищий за знак, тож звичайна мітка стоїть трохи криво */
static gboolean dr_icon_draw(GtkWidget *da, cairo_t *cr, GtkWidget *tab) {
    const char *icon = g_object_get_data(G_OBJECT(tab), "icon");
    PangoLayout *l = gtk_widget_create_pango_layout(da, icon ? icon : "");
    PangoRectangle ink;
    pango_layout_get_pixel_extents(l, &ink, NULL);
    int w = gtk_widget_get_allocated_width(da), h = gtk_widget_get_allocated_height(da);
    GdkRGBA col;
    GtkStyleContext *sc = gtk_widget_get_style_context(da);
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &col);
    gdk_cairo_set_source_rgba(cr, &col);
    cairo_move_to(cr, (w - ink.width) / 2.0 - ink.x, (h - ink.height) / 2.0 - ink.y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return TRUE;
}
void drawer_card_init(Card *c, const char *cls, const char *icon, const char *tip, gboolean bottom) {
    DrGroup *g = &dr_grp[bottom ? 1 : 0];
    if (!g->win) dr_group_build(g, bottom);
    c->in_drawer = TRUE;
    c->win = g->win;                               /* вікно групи: клавіатура для полів, зміна розміру */
    c->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(c->card, "card");
    add_class(c->card, cls);
    add_class(c->card, "dr-panel");
    gtk_widget_set_size_request(c->card, CARD_W, -1);
    c->rev = gtk_scrolled_window_new(NULL, NULL);  /* «вікно» висування: ширина 0…CARD_W, без смуг прокрутки */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->rev), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(c->rev), TRUE);
    gtk_container_add(GTK_CONTAINER(c->rev), c->card);
    g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(c->rev)), "changed", G_CALLBACK(dr_stick_right), NULL);
    gtk_widget_set_no_show_all(c->rev, TRUE);      /* показується лише висунутою; вміст — show_all(c->card) */
    dr_width(c, 0);
    gtk_box_pack_start(GTK_BOX(g->panels), c->rev, FALSE, FALSE, 0);
    c->tab = gtk_button_new();                     /* кружок; значок малюємо самі — точно по центру (dr_icon_draw) */
    add_class(c->tab, "dr-tab");
    GtkWidget *da = gtk_drawing_area_new();
    g_object_set_data_full(G_OBJECT(c->tab), "icon", g_strdup(icon), g_free);
    g_signal_connect(da, "draw", G_CALLBACK(dr_icon_draw), c->tab);
    gtk_container_add(GTK_CONTAINER(c->tab), da);
    gtk_widget_show(da);
    gtk_widget_set_can_focus(c->tab, FALSE);
    gtk_widget_set_tooltip_text(c->tab, tip);
    gtk_widget_set_no_show_all(c->tab, TRUE);      /* видно, якщо не вимкнено (.off) — див. cards_apply */
    g_signal_connect(c->tab, "clicked", G_CALLBACK(on_tab), c);
    gtk_box_pack_start(GTK_BOX(g->tabs), c->tab, FALSE, FALSE, 0);   /* у порядку виклику, згори вниз */
    if (dr_n < (int)G_N_ELEMENTS(dr_cards)) dr_cards[dr_n++] = c;
}
void drawer_tab_icon(Card *c, const char *icon, const char *state_cls) {
    if (!c->tab) return;
    if (icon && g_strcmp0(g_object_get_data(G_OBJECT(c->tab), "icon"), icon)) {
        g_object_set_data_full(G_OBJECT(c->tab), "icon", g_strdup(icon), g_free);
        gtk_widget_queue_draw(c->tab);
    }
    GtkStyleContext *sc = gtk_widget_get_style_context(c->tab);
    gtk_style_context_remove_class(sc, "on");
    gtk_style_context_remove_class(sc, "off");
    if (state_cls) gtk_style_context_add_class(sc, state_cls);
}

/* Прямокутник картки на моніторі — для hw_visible (картка прив'язана до правого й верхнього країв) */
static HwRect card_rect(HwRect m, gpointer d) {
    Card *c = d;
    double w = gtk_widget_get_allocated_width(c->win), h = gtk_widget_get_allocated_height(c->win);
    double x = c->centered ? m.x + (m.w - w) / 2 : m.x + m.w - c->shown_right - w;
    return (HwRect){ x, m.y + c->shown_top, w, h };
}
gboolean card_visible(Card *c) {
    if (c->in_drawer) {                            /* висувна: групу видно й саме ця панель висунута */
        DrGroup *g = &dr_grp[c->win == dr_grp[1].win];
        return gtk_widget_get_visible(g->win) && gtk_widget_get_visible(c->rev)
            && dr_anim(c)->pos >= 1 && hw_visible(card_rect, &g->pc);
    }
    return gtk_widget_get_visible(c->win) && hw_visible(card_rect, c);
}

void hypr_eval(const char *lua) {
    char *argv[] = { "hyprctl", "eval", (char *)lua, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

/* ═════════════ ЗАПУСК: які віджети показати ═════════════ */
static Card *CARDS[] = { &cal, &mus, &wth, &mix, &ntf, &bat, &wfi, &blt, &sysm, &dev, &gms, &outc,
#ifdef HAVE_VTE
                         &trm,                     /* термінал — лише якщо зібрано з vte3 */
#endif
};   /* геймпади — свої картки, див. pads.c */

gboolean cards_apply(gpointer d) {                 /* старт, SIGHUP (desk.sh toggle …) і зміна вмісту (.empty) */
    for (guint i = 0; i < G_N_ELEMENTS(CARDS); i++) {
        Card *c = CARDS[i];
        if (!c->win) continue;                     /* ще не створена */
        if (c->in_drawer) {                        /* висувна: ховається лише язичок */
            gboolean want = enabled(c) && !c->empty;
            gtk_widget_set_visible(c->tab, want);
            if (!want) dr_set(c, FALSE, FALSE);
            continue;
        }
        gboolean want = enabled(c) && !c->empty, now = gtk_widget_get_visible(c->win);
        if (want && !now)      { gtk_widget_show_all(c->win); if (c->shown) c->shown(); }
        else if (!want && now) { gtk_widget_hide(c->win);     if (c->hidden) c->hidden(); }
    }
    for (int k = 0; k < 2; k++) {                  /* група шторки — якщо в ній є хоч один язичок */
        if (!dr_grp[k].win) continue;
        gboolean any = FALSE;
        for (int i = 0; i < dr_n; i++) if (dr_cards[i]->win == dr_grp[k].win) any |= gtk_widget_get_visible(dr_cards[i]->tab);
        gtk_widget_set_visible(dr_grp[k].win, any);
    }
    pads_apply();                                  /* картки геймпадів */
    tablet_apply();                                /* картки планшетів */
    cards_relayout();
    return G_SOURCE_CONTINUE;
}

static gboolean on_quit(gpointer d) { gtk_main_quit(); return G_SOURCE_REMOVE; }

int main(int argc, char **argv) {
    hw_single(g_getenv("DESK_TEST_OVERLAY") ? "desk-test" : "desk",       /* тестова копія поверх усього — окремо */
              (const int[]){ SIGHUP, SIGUSR1, SIGUSR2, 0 });          /* обробники — нижче, після запуску GTK */
    gtk_init(&argc, &argv);
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) return 1;
    char *theme = g_build_filename(g_get_home_dir(), ".config/theme/gen/theme.css", NULL);
    char *all = g_strdup_printf("@import url(\"file://%s\");\n%s%s", theme, CSS,   /* тема + картки */
                                g_getenv("DESK_TEST_SOLID") ? ".ui-card { background-color: @bg; }\n" : "");   /* знімки для перевірки */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, all, -1, NULL);
    g_free(all); g_free(theme);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    load_pos(&cal);                                /* спершу: решта карток за замовчуванням — під календарем */
    cal_init();
    music_init();
    weather_init();                                /* під музикою */
    mixer_init();                                  /* під погодою */
    notify_init();                                 /* ліворуч від календаря, на всю висоту */
    pads_init();                                   /* геймпади — лише коли підключені */
    tablet_init();                                 /* графічні планшети — лише коли підключені */
    battery_init();                                /* заряд — лише коли є бездротові пристрої */
    wifi_init();                                   /* Wi-Fi — висувна панель, язичок згори */
    bt_init();                                     /* Bluetooth — висувна, під Wi-Fi */
    sys_init();                                    /* система */
    games_init();                                  /* ігри — висувна, нижня група, над Dev-стеком */
    dev_init();                                    /* Dev-стек — висувна, язичок у самому низу */
    devconn_init();                                /* «підключити планшет / контролер» — коли не підключені (після bt_init) */
    out_init();                                    /* вихід звуку */
#ifdef HAVE_VTE
    term_init();                                   /* термінал на столі */
#endif

    cards_apply(NULL);
    const char *t_open = g_getenv("DESK_TEST_DRAWER");         /* знімки для перевірки: висунути панель одразу */
    for (int i = 0; t_open && i < dr_n; i++) {
        char *nm = g_strndup(dr_cards[i]->off_name, strcspn(dr_cards[i]->off_name, "."));
        char **want = g_strsplit(t_open, ",", -1);
        if (g_strv_contains((const char *const *)want, nm)) dr_set(dr_cards[i], TRUE, FALSE);
        g_strfreev(want); g_free(nm);
    }
    g_unix_signal_add(SIGHUP, cards_apply, NULL);
    edges_init();
    g_unix_signal_add(SIGTERM, on_quit, NULL);
    g_unix_signal_add(SIGINT, on_quit, NULL);
    gtk_main();
    return 0;
}
