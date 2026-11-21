#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  УНІВЕРСАЛЬНЕ МЕНЮ (SUPER)                                   │
# │  Ліворуч — пошук, праворуч — панель «top».                   │
# │  Символ на початку вибирає, що шукати:                       │
# │    (нічого)  програми, вікна, швидкі дії                     │
# │    @ команда → виконати в терміналі     (укр. розкладка: ")  │
# │    / запит   → шукати в інтернеті       (укр. розкладка: .)  │
# │    %         → лише швидкі дії (нічне світло, знімок…)       │
# │    ' назва   → файли з домашньої теки   (укр.: клавіша ' ~)  │
# │  Рядок-підказка завжди каже, що зробить Enter:               │
# │    22 + 5 → = результат · github.com → браузер ·             │
# │    ~/шлях → показати у теці · команда → термінал ·           │
# │    інше → пошук в інтернеті.                                 │
# │  Shift+Enter на файлі — показати у теці.                     │
# │  Невиконаний минулий запит (Esc, недописав) стоїть у полі    │
# │  звичайним текстом — список одразу фільтрується, можна       │
# │  дописувати. Щось виконав — поле наступного разу порожнє.    │
# │  Порожнє поле — лише відкриті вікна (програми — коли щось    │
# │  надрукуєш).  Ctrl+A — виділити все, Ctrl+C — копіювати,     │
# │  Ctrl+V — вставити.                                          │
# ╰──────────────────────────────────────────────────────────────╯

S="$HOME/.config/rofi/scripts/universal.py"
PLUGIN_DIR="$HOME/.local/lib/rofi"
LAST="${XDG_CACHE_HOME:-$HOME/.cache}/rofi/uni-last.txt"

# Підрежими (порядок = порядок у списку). Через кому; "назва:команда" — скрипт-режим.
export UNI_MODES="window,drun,acts:$S acts,files:$HOME/.config/rofi/scripts/files.sh"

# Якщо плагін ще не зібрано (або rofi оновився) — зібрати
if [ ! -f "$PLUGIN_DIR/uni.so" ] || [ "$(command -v rofi)" -nt "$PLUGIN_DIR/uni.so" ]; then
    "$HOME/.config/hypr/src/build.sh" uni >/dev/null 2>&1
fi

# Невиконаний минулий запит → у поле (виконаний плагін стирає)
filter=()
[ -s "$LAST" ] && filter=(-filter "$(cat "$LAST")")

# Розкладка: [ пошук ] зверху, під ним [ список | панель «top» ]
layout='
window   { width: 1100px; }
mainbox  { children: [ inputbar, content ]; }
content  { orientation: horizontal; spacing: 14px; children: [ listview, message ]; }
listview { lines: 11; expand: true; }
message  { expand: true; padding: 14px 16px; border-radius: 10px; background-color: @bg; }
message textbox { font: @font-body; vertical-align: 0; }
'

# Кеш списку програм: перечитати, якщо після нього щось встановили / видалили
# (тека з ярликами .desktop новіша за кеш). Інакше — швидкий старт з кешу.
DRUN_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/rofi-drun-desktop.cache"
reload=()
IFS=: read -ra data_dirs <<< "${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:$HOME/.local/share"
for d in "${data_dirs[@]}"; do
    if [ -d "$d/applications" ] && { [ ! -f "$DRUN_CACHE" ] || [ "$d/applications" -nt "$DRUN_CACHE" ]; }; then
        reload=(-drun-reload-desktop-cache)
        break
    fi
done

export ROFI_PLUGIN_PATH="$PLUGIN_DIR"
exec rofi -show uni -modi uni "${reload[@]}" \
    -display-uni $' ' \
    -theme-str 'mode-switcher { enabled: false; }' \
    -theme-str 'entry { placeholder: "Програми   ·   @ термінал   ·   / інтернет   ·   % дії   ·   '"'"' файли"; }' \
    -theme-str "$layout" \
    "${filter[@]}" \
    "$@"
