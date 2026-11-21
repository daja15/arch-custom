/* DESK — спільне для віджетів робочого столу (див. desk.c) */
#pragma once
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gtk-layer-shell.h>
#include <glib-unix.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hyprwatch.h"
#include "theme.h"

#define GRID        20       /* крок сітки при перетягуванні (px) */
#define CARD_W      284      /* ширина карток */
#define SYS_W       344      /* ширина картки «Система» (sysmon.c) — ширша, бо довгі рядки */
#define CAL_RIGHT   60       /* календар за замовчуванням: відступ справа */
#define CAL_TOP     140      /* … і згори */
#define CAL_H       280      /* висота календаря (для місця наступних карток під ним) */

/* Вікно-картка, що перетягується по сітці. Кожен віджет має свою. */
typedef struct {
    GtkWidget *win, *card;
    const char *pos_name;        /* файл позиції в ~/.cache */
    const char *off_name;        /* прапорець «сховано» в ~/.config/hypr */
    int mright, mtop;
    gboolean dragging;
    double press_x, press_y;
    void (*moved)(void);         /* після перетягування (може бути NULL) */
    void (*shown)(void);         /* картку щойно показали / сховали (може бути NULL) */
    void (*hidden)(void);
    gboolean centered;           /* по центру екрана по горизонталі (тягнеться лише вгору-вниз) */
    gboolean empty;              /* показувати нічого (напр., немає геймпадів) — картку сховано */
    int shown_top, shown_right;  /* де картку видно насправді (не влазить / налазить на іншу — зсунута) */
    /* висувна панель біля «Сповіщень» (Wi-Fi, Bluetooth, Dev-стек): своє вікно не створюється,
     * win — спільне вікно панелей, картка — у висувній шторці, відкривається язичком */
    gboolean in_drawer;
    GtkWidget *rev, *tab;
} Card;

/* desk.c — картки */
int      snap(int v);
void     add_class(GtkWidget *w, const char *c);
gboolean load_pos(Card *c);
void     card_init(Card *c, const char *ns, const char *cls);
void     hypr_eval(const char *lua);
gboolean card_visible(Card *c);
void     cards_unregister(Card *c);   /* перед знищенням динамічної картки (геймпади, планшет) */
void     cards_relayout(void);  /* картки змінили розмір — розсунути, щоб не налазили */
/* Висувна панель збоку «Сповіщень»: язичок зі значком; клік — висунути картку, ще клік — сховати.
 * bottom — язичок унизу (Dev-стек), інакше — згори (у порядку виклику) */
void     drawer_card_init(Card *c, const char *cls, const char *icon, const char *tip, gboolean bottom);
void     drawer_tab_icon(Card *c, const char *icon, const char *state_cls);   /* значок / стан на язичку */
void     ui_list_fit(GtkWidget *scroll, GtkWidget *list, int max_rows);       /* висота списку — до max_rows рядків, далі прокрутка */
/* Будова карток (див. UI-кіт у gen/theme.css): заголовок, рядок списку, дії */
GtkWidget *ui_header(Card *c, const char *icon, const char *title, GtkWidget **title_out, GtkWidget **sub_out);
                                    /* іконка 24 + назва (+ підпис, якщо sub_out); керування — gtk_box_pack_end(head, …) */
GtkWidget *ui_lead(const char *icon);                  /* колонка іконки 24 px */
GtkWidget *ui_text(const char *primary, const char *secondary, GtkWidget **p_out, GtkWidget **s_out);
                                    /* назва + підпис під нею (secondary NULL — без підпису) */
GtkWidget *ui_row_box(GtkWidget *lead, GtkWidget *text);   /* [іконка] 12 [текст] — дії додавати pack_end */
GtkWidget *ui_section(GtkWidget *box, const char *title);  /* назва розділу */
GtkWidget *ui_actions(GtkWidget *box);                     /* рядок кнопок унизу картки: кнопки нарівно */     /* картку показано й НЕ закрито непрозорим вікном (kitty — прозорий) */
gboolean cards_apply(gpointer d);   /* показати / сховати картки за .off-файлами та .empty */
extern GDBusConnection *bus;

/* віджети: картка + запуск */
extern Card cal, mus, wth, mix, ntf, bat, blt, sysm, dev, gms, outc, wfi;
#ifdef HAVE_VTE
extern Card trm;
void term_init(void);       /* term.c    — термінал на столі (потрібен vte3) */
#endif
void cal_init(void);       /* cal.c     — календар */
void music_init(void);     /* music.c   — що грає, хвиля */
void weather_init(void);   /* weather.c — погода */
void mixer_init(void);     /* mixer.c   — мікшер звуку */
void notify_init(void);    /* notify.c  — історія сповіщень */
void edges_init(void);     /* edges.c   — гарячі краї екрана */
void battery_init(void);   /* battery.c — заряд бездротових пристроїв (крім геймпадів) */
void bt_init(void);        /* bt.c      — Bluetooth: увімкнути, пошук, назви, сила сигналу */
void pads_init(void);      /* pads.c    — підключені геймпади: кожен — окрема картка (кнопки, стіки, курки, гіроскоп) */
void sys_init(void);       /* sysmon.c  — процесор (+t°), графіка, пам'ять, диск, живлення */
void dev_init(void);       /* dev.c     — Dev-стек: Docker + проєкт */
void wifi_init(void);      /* wifi.c    — Wi-Fi: мережі поруч, підключення (NetworkManager) */
void out_init(void);       /* out.c     — вихід звуку: поле вибору пристрою */
void games_init(void);     /* games.c   — ігри: на цьому ПК і останні в Steam */
void pads_apply(void);     /* pads.c    — перечитати pads.off (SIGHUP) */
void tablet_init(void);    /* tablet.c  — графічний планшет: перо, натиск, кнопки, налаштування */
void tablet_apply(void);   /* tablet.c  — перечитати tablet.off (SIGHUP) */
void devconn_init(void);   /* devconn.c — «підключити»: планшет / контролери, коли не підключені */
void devconn_online(const char *kind, gboolean on);   /* "tablet" | "gaming": жива картка є — «підключити» сховати */
void devconn_refresh(void);
GtkWidget *btlink_new(const char *usb_name, const char *mac, const char *kind);   /* у картку пристрою на кабелі: «Сполучити по Bluetooth» */
void     btlink_unplugged(const char *usb_name, const char *mac);               /* кабель витягнули — підключити по Bluetooth */
/* bt.c — для інших карток */
extern void (*bt_say_hook)(const char *msg);           /* повідомлення Bluetooth (помилки, «шукаю…») */
gboolean bt_device_act(const char *path);              /* як клік у списку: сполучити / підключити (з пошуком загубленого) */
void     bt_scan_set(gboolean on);
gboolean bt_scanning(void);
gboolean bt_device_busy(const char *path);