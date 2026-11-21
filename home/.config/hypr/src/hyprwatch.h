/*
 * ╭──────────────────────────────────────────────────────────────╮
 * │  HYPRWATCH — чи видно віджет робочого столу                  │
 * │  Спільне для еквалайзера (visualizer) і віджетів (desk).     │
 * │                                                              │
 * │  • hw_listen(cb) — події Hyprland (socket2): вікна й робочі  │
 * │    столи змінились → cb() (кілька подій поспіль → один       │
 * │    виклик через 120 мс). Без опитування.                     │
 * │  • hw_visible(rect_fn, data) — чи НЕ закриває прямокутник    │
 * │    віджета непрозоре вікно. rect_fn отримує монітор і        │
 * │    повертає прямокутник віджета на ньому.                    │
 * │    Вікна kitty — прозорі: крізь них віджет видно.            │
 * │  • hw_tick_start(fps, cb) — таймер анімації на СПІЛЬНІЙ       │
 * │    «сітці кадрів»: спрацьовує в моменти k/fps секунди за     │
 * │    монотонним годинником (однаковим для всіх програм).       │
 * │    Еквалайзер і хвиля музики малюють в один момент →         │
 * │    Hyprland складає екран один раз, а не двічі.              │
 * │    cb повертає TRUE — далі, FALSE — зупинитись. Іде, поки    │
 * │    tick.src != 0.                                            │
 * │  • hw_single(name, signals) — на самому початку main():      │
 * │    друга копія програми одразу виходить (замок у             │
 * │    $XDG_RUNTIME_DIR), а перелічені сигнали ігноруються, поки │
 * │    програма не поставить свої обробники (інакше, напр.,      │
 * │    ранній SIGHUP просто вбив би її під час старту).          │
 * ╰──────────────────────────────────────────────────────────────╯
 */
#pragma once
#include <glib.h>

typedef struct { double x, y, w, h; } HwRect;   /* у логічних пікселях екрана */

/* Монітор (x, y, w, h) → прямокутник віджета на ньому */
typedef HwRect (*HwRectFn)(HwRect monitor, gpointer data);

void     hw_listen(GSourceFunc on_change);
/* Таймер на сітці кадрів (оголоси static HwTick t; — і hw_tick_start(&t, 24, cb)) */
typedef struct { gint64 period; GSourceFunc cb; guint src; } HwTick;
void     hw_tick_start(HwTick *t, int fps, GSourceFunc cb);   /* уже йде — нічого */
void     hw_tick_stop(HwTick *t);
gboolean hw_visible(HwRectFn rect_fn, gpointer data);
void     hw_single(const char *name, const int *signals);   /* signals — масив, що закінчується 0 */
