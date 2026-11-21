#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  НІЧНЕ СВІТЛО — увімкнути / вимкнути (клік по іконці waybar)│
# │  Увімкнено = працює hyprsunset з теплими кольорами.          │
# │  Вимкнено  = hyprsunset зупинено, кольори звичайні.          │
# │  Тепліше / холодніше — змінити TEMP нижче                     │
# │  (менше = тепліше: 6500 звичайно, 4500 тепло, 3500 дуже).    │
# ╰──────────────────────────────────────────────────────────────╯
TEMP=4500

if pgrep -x hyprsunset >/dev/null; then
    pkill -x hyprsunset
    for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -x hyprsunset >/dev/null || break; sleep 0.1; done   # дочекатися виходу
    notify-send -a "Нічне світло" -t 1500 "Нічне світло: вимкнено"
else
    setsid -f hyprsunset -t "$TEMP" >/dev/null 2>&1
    notify-send -a "Нічне світло" -t 1500 "Нічне світло: увімкнено (${TEMP}K)"
fi
pkill -RTMIN+13 -x waybar     # оновити іконку в waybar
