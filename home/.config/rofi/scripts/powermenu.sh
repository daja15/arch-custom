#!/usr/bin/env bash
# ╭──────────────────────────────────────────────╮
# │  Меню живлення на rofi (тема «Тиха ніч»)    │
# │  Виклик: SUPER+Esc або кнопка ⏻ у waybar     │
# ╰──────────────────────────────────────────────╯

# Пункти меню (іконка + підпис). Порядок = порядок у меню.
lock=$'  Заблокувати'
suspend=$'  Сон'
logout=$'  Вийти з сесії'
reboot=$'  Перезавантажити'
shutdown=$'  Вимкнути'

uptime_txt="Увімкнено: $(uptime -p | sed 's/^up //')"

choice=$(printf '%s\n' "$lock" "$suspend" "$logout" "$reboot" "$shutdown" | rofi -dmenu \
    -p $'  Живлення' \
    -mesg "$uptime_txt" \
    -no-custom \
    -i \
    -theme-str 'window { width: 340px; }' \
    -theme-str 'listview { lines: 5; }' \
    -theme-str 'mode-switcher { enabled: false; }' \
    -theme-str 'entry { placeholder: "Оберіть дію…"; }')

case "$choice" in
    "$lock")     hyprlock ;;                                 # блокування екрана
    "$suspend")  hyprlock & sleep 0.5; systemctl suspend ;;  # спершу блокуємо, тоді сон
    "$logout")   hyprctl dispatch 'hl.dsp.exit()' ;;         # вихід з Hyprland
    "$reboot")   systemctl reboot ;;                         # перезавантаження
    "$shutdown") systemctl poweroff ;;                       # вимкнення
esac
