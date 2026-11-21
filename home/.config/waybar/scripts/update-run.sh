#!/bin/sh
# ╭──────────────────────────────────────────────────────────────╮
# │  Оновити систему в терміналі (клік по модулю оновлень у      │
# │  waybar), потім оновити лічильник                            │
# ╰──────────────────────────────────────────────────────────────╯
kitty --single-instance --instance-group=term --wait-for-single-instance-window-close \
    --class floating-update -e sh -c 'yay -Syu; echo; printf "Готово. Enter — закрити"; read _'
pkill -RTMIN+12 -x waybar
