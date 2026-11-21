#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  CTRL+ALT+DEL — меню режимів ноутбука (тема «Тиха ніч»)      │
# │  Ряд великих іконок на весь екран, на фоні робочого столу:   │
# │    󰐥 Вимкнути   󰜉 Перезавантажити   󰜗 Гібернація   󰘚 BIOS     │
# │    󰨇 Диспетчер завдань (btop)   󰌾 Заблокувати                │
# │  Гібернація — пам'ять на диск і вимкнення; потрібен swap не  │
# │  менший за пам'ять. Не налаштована — покаже плашку з причиною.│
# │  ← → — вибір, Enter / клік — виконати, Esc — закрити.        │
# │  Одразу вибрано найлівішу плитку — «Вимкнути» (Enter = вимкнути).│
# │  Вигляд — ~/.config/rofi/ctrlaltdel.rasi                     │
# ╰──────────────────────────────────────────────────────────────╯

# Повторне Ctrl+Alt+Del — закрити меню
pkill -x rofi && exit 0

# Кольори теми (TH_BG тощо) — фон меню
. "$HOME/.config/theme/gen/theme.sh"
ALPHA="e6"    # непрозорість фону: e6 ≈ 90% — робочий стіл ледь проглядає

# Плитка: велика іконка + підпис під нею (розмітка Pango)
tile() {   # tile <іконка> <підпис>
    printf "<span font='JetBrainsMono Nerd Font Mono 40'>%s</span>\n<span font='JetBrainsMono Fast 11'>%s</span>|" "$1" "$2"
}

items=$(
    tile "󰐥" "Вимкнути"
    tile "󰜉" "Перезавантажити"
    tile "󰜗" "Гібернація"
    tile "󰘚" "BIOS"
    tile "󰨇" "Диспетчер"
    tile "󰌾" "Заблокувати"
)

# Скільки працює ноутбук — українською (uptime -p пише лише англійською)
secs=$(cut -d. -f1 /proc/uptime)
h=$(( secs / 3600 )); m=$(( secs % 3600 / 60 ))
if [ "$h" -gt 0 ]; then up="Працює ${h} год ${m} хв"; else up="Працює ${m} хв"; fi

choice=$(printf '%s' "${items%|}" | rofi -dmenu \
    -theme "$HOME/.config/rofi/ctrlaltdel.rasi" \
    -theme-str "window { background-color: ${TH_BG}${ALPHA}; }" \
    -theme-str 'inputbar { enabled: false; }' \
    -sep '|' -eh 5 -markup-rows \
    -no-custom -format i \
    -selected-row 0 \
    -mesg "$up   ·   ← → вибір   ·   Enter виконати   ·   Esc закрити" \
    -kb-move-char-back '' -kb-move-char-forward '' \
    -kb-row-left 'Left' -kb-row-right 'Right')

case "$choice" in
    0) systemctl poweroff ;;                                 # вимкнути
    1) systemctl reboot ;;                                   # перезавантажити
    2) # гібернація: спершу блокуємо (після пробудження — екран пароля), тоді пам'ять → диск
       can=$(busctl call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager CanHibernate 2>/dev/null)
       if [ "$can" = 's "yes"' ]; then
           hyprlock & sleep 0.5; systemctl hibernate
       else
           swayosd-client --custom-message "Гібернація не налаштована: немає swap" --custom-icon dialog-warning >/dev/null 2>&1
       fi ;;
    3) systemctl reboot --firmware-setup ;;                  # перезавантажити одразу в BIOS (UEFI)
    4) kitty --class taskmgr --title "Диспетчер завдань" -e btop ;;   # процеси, пам'ять, процесор
    5) hyprlock ;;                                           # заблокувати екран
esac
