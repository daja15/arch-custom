#!/bin/sh
# ╭──────────────────────────────────────────────────────────────╮
# │  Піпетка — SUPER+Shift+C: клацни будь-де на екрані, колір    │
# │  (#hex) скопіюється в буфер обміну                           │
# ╰──────────────────────────────────────────────────────────────╯
if ! command -v hyprpicker >/dev/null; then
    notify-send -a "Піпетка" "hyprpicker не встановлено" "sudo pacman -S hyprpicker"; exit 1
fi
c=$(hyprpicker -a -f hex) || exit 0          # -a — одразу скопіювати; Esc — скасувати
[ -n "$c" ] && notify-send -a "Піпетка" "Колір скопійовано: $c" "<span background='$c'>                    </span>"
