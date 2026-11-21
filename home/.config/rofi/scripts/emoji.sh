#!/bin/sh
# ╭──────────────────────────────────────────────────────────────╮
# │  Емодзі — SUPER+. : пошук (англійською назвою: heart, fire,  │
# │  smile…), Enter — скопіювати в буфер обміну                  │
# ╰──────────────────────────────────────────────────────────────╯
if ! rofi -help 2>/dev/null | grep -q '^ *emoji\b' && [ ! -e /usr/lib/rofi/emoji.so ]; then
    notify-send -a "Емодзі" "rofi-emoji не встановлено" "sudo pacman -S rofi-emoji"; exit 1
fi
ROFI_PLUGIN_PATH=/usr/lib/rofi exec rofi -modi emoji -show emoji -emoji-mode copy \
    -theme-str 'window { width: 640px; }' \
    -theme-str 'mode-switcher { enabled: false; }' \
    -theme-str 'entry { placeholder: "Пошук емодзі (англійською)…"; }'
