#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  ІСТОРІЯ БУФЕРА ОБМІНУ — SUPER+Shift+V (cliphist + rofi)     │
# │  Enter — скопіювати вибране знову (далі Ctrl+V куди треба)   │
# │  Alt+Delete — видалити запис · Alt+Shift+Delete — очистити все│
# │  Записує історію hypr/services.sh (wl-paste --watch).        │
# ╰──────────────────────────────────────────────────────────────╯
if ! command -v cliphist >/dev/null; then
    notify-send -a "Буфер обміну" "cliphist не встановлено" "sudo pacman -S cliphist"; exit 1
fi
sel=$(cliphist list | rofi -dmenu -i -display-columns 2 \
    -p $'  Буфер обміну' \
    -mesg "Enter — скопіювати · Alt+Delete — видалити · Alt+Shift+Delete — очистити все" \
    -kb-custom-1 "Alt+Delete" -kb-custom-2 "Alt+Shift+Delete" \
    -theme-str 'window { width: 720px; }' \
    -theme-str 'listview { lines: 12; }' \
    -theme-str 'mode-switcher { enabled: false; }' \
    -theme-str 'entry { placeholder: "Пошук у скопійованому…"; }')
code=$?
case $code in
    0)  [ -n "$sel" ] && printf '%s' "$sel" | cliphist decode | wl-copy ;;
    10) [ -n "$sel" ] && printf '%s' "$sel" | cliphist delete && exec "$0" ;;   # видалити й показати знову
    11) cliphist wipe && notify-send -a "Буфер обміну" "Історію очищено" ;;
esac
