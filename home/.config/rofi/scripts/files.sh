#!/bin/sh
# Файли для універсального меню (символ ').
# Список — готовий кеш ~/.cache/rofi/files.rofi: просто вивести його (без запуску Python).
# Вибір рядка, перший запуск без кешу або застарілий кеш — передати universal.py.
# Кеш оновлює таймер: systemctl --user status rofi-files.timer
IDX="${XDG_CACHE_HOME:-$HOME/.cache}/rofi/files.rofi"
if [ "${ROFI_RETV:-0}" = 0 ] && [ -s "$IDX" ] && [ -n "$(find "$IDX" -mmin -1440)" ]; then
    exec cat "$IDX"
fi
exec "$HOME/.config/rofi/scripts/universal.py" files "$@"
