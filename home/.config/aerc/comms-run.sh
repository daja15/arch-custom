#!/bin/sh
# Пошта в правій панелі (hypr/comms-launch.sh mail).
# Перший раз — налаштування скриньок (comms-mail-setup), далі — одразу aerc.
# Кнопка «Пошта» в рядку «Додати акаунт» меню створює прапорець і закриває aerc —
# тоді тут додається ще одна скринька (comms-mail-setup --add), а потім знову aerc.
FLAG="${XDG_RUNTIME_DIR:-/tmp}/mail-add-account"
printf '\033]2;Пошта\007'   # заголовок вікна
if [ ! -s "$HOME/.config/aerc/accounts.conf" ]; then
    rm -f "$FLAG"
    "$HOME/.local/bin/comms-mail-setup" || { echo; echo "Налаштування не завершено. Натисни Enter, щоб закрити."; read -r _; exit 1; }
fi
while :; do
    if [ -e "$FLAG" ]; then
        rm -f "$FLAG"
        clear
        "$HOME/.local/bin/comms-mail-setup" --add || { echo; echo "Скриньку не додано. Enter — повернутись до пошти."; read -r _; }
    else
        aerc
    fi
    [ -e "$FLAG" ] || break   # прапорця немає — це був звичайний вихід
done
