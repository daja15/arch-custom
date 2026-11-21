#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  WI-FI — меню в стилі SUPER (клік по мережі у waybar)        │
# │  Вгорі — увімкнути / вимкнути Wi-Fi, нижче — мережі за       │
# │  силою сигналу (● — підключена). Enter: знайома мережа —     │
# │  підключитися; нова із захистом — спитає пароль.             │
# │  Працює через NetworkManager (nmcli).                        │
# ╰──────────────────────────────────────────────────────────────╯
menu() { rofi -dmenu -i -no-custom -p "$1" -theme-str 'window { width: 520px; }' \
              -theme-str 'listview { lines: 12; }' -theme-str 'mode-switcher { enabled: false; }' "${@:2}"; }
note() { notify-send -a "Wi-Fi" -t 3000 "$@"; }

on=$(nmcli -t radio wifi)
if [ "$on" != enabled ]; then
    [ "$(printf '󰖩  Увімкнути Wi-Fi' | menu ' Wi-Fi вимкнено')" ] && nmcli radio wifi on && note "Wi-Fi увімкнено"
    exit
fi
list=$(nmcli -t -f IN-USE,SIGNAL,SECURITY,SSID dev wifi list --rescan auto 2>/dev/null \
       | awk -F: '$4 != "" && !seen[$4]++ { printf "%s  %3s%%  %s  %s\n", ($1=="*"?"●":" "), $2, ($3==""?" ":"󰌾"), $4 }')
choice=$(printf '󰖪  Вимкнути Wi-Fi\n%s' "$list" | menu ' Wi-Fi' -mesg "● — підключено · 󰌾 — з паролем")
[ -z "$choice" ] && exit
if [ "$choice" = "󰖪  Вимкнути Wi-Fi" ]; then nmcli radio wifi off && note "Wi-Fi вимкнено"; exit; fi
ssid=$(printf '%s' "$choice" | sed -E 's/^.{1}  +[0-9]+%  .  //')
if nmcli -t -f NAME con show | grep -qxF "$ssid"; then          # знайома мережа
    nmcli con up id "$ssid" >/dev/null 2>&1 && note "Підключено: $ssid" || note "Не вдалося підключитися" "$ssid"
elif printf '%s' "$choice" | grep -q "󰌾"; then                  # нова, з паролем
    pass=$(rofi -dmenu -password -p "Пароль · $ssid" -theme-str 'window { width: 520px; }' \
                -theme-str 'listview { lines: 0; }' -theme-str 'mode-switcher { enabled: false; }' </dev/null)
    [ -z "$pass" ] && exit
    nmcli dev wifi connect "$ssid" password "$pass" >/dev/null 2>&1 && note "Підключено: $ssid" || note "Не вдалося підключитися" "Перевір пароль · $ssid"
else
    nmcli dev wifi connect "$ssid" >/dev/null 2>&1 && note "Підключено: $ssid" || note "Не вдалося підключитися" "$ssid"
fi
