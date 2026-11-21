#!/bin/sh
# Запуск nchat у правій панелі (hypr/comms.lua).
# Кнопка «+» у меню створює прапорець і закриває nchat — тоді тут
# запускається додавання акаунта (nchat --setup), а потім знову nchat.
# Звичайний вихід (Ctrl+Q) — вікно закривається.
FLAG="${XDG_RUNTIME_DIR:-/tmp}/nchat-add-account"
printf '\033]2;nchat\007'   # заголовок вікна

# nchat уже відкритий в іншому вікні — друга копія не запуститься (профіль заблоковано)
while pgrep -x nchat >/dev/null; do
    clear
    echo "nchat уже відкритий в іншому вікні."
    echo "Закрий його там (Ctrl+Q) і натисни Enter — він відкриється тут."
    read -r _ || exit 0
done

THEME_COLORS="$HOME/.config/theme/gen/nchat-color.conf"   # кольори поточної теми (~/.config/theme/apply)

while :; do
    [ -f "$THEME_COLORS" ] && cp -f "$THEME_COLORS" "$HOME/.config/nchat/color.conf"   # nchat читає їх лише при старті
    if [ -e "$FLAG" ]; then
        rm -f "$FLAG"
        clear
        echo "Додавання акаунта: вибери протокол (Telegram / WhatsApp), далі номер +380… і код з Telegram."
        echo
        nchat --setup
    else
        nchat
    fi
    [ -e "$FLAG" ] || break   # прапорця немає — це був звичайний вихід
done
