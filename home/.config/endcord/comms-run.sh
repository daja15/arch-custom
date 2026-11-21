#!/bin/sh
# Запуск endcord (Discord) у правій панелі «Зв'язок» (hypr/comms.lua, comms-launch.sh).
# Перший запуск — вхід: QR-код з телефона (найпростіше), пошта+пароль або токен.
# Кнопка «Discord» у рядку «Додати акаунт» меню створює прапорець і закриває endcord —
# тоді тут відкривається менеджер профілів endcord (додати / вибрати акаунт),
# а після вибору одразу запускається Discord з цим акаунтом.
# Звичайний вихід (Ctrl+C) — вікно закривається.
FLAG="${XDG_RUNTIME_DIR:-/tmp}/discord-add-account"
printf '\033]2;Discord\007'   # заголовок вікна

while :; do
    if [ -e "$FLAG" ]; then
        rm -f "$FLAG"
        clear
        echo "Акаунти Discord: «Add» — додати нового (QR-код з телефона найпростіше),"
        echo "або вибери наявний стрілками й Enter."
        sleep 1
        endcord --manager
    else
        endcord
    fi
    [ -e "$FLAG" ] || break   # прапорця немає — це був звичайний вихід
done
