#!/bin/sh
# ╭──────────────────────────────────────────────────────────────╮
# │  ФОНОВІ СЛУЖБИ — запускаються з hyprland.lua при старті та   │
# │  після перечитування конфігу. Кожна — лише якщо встановлена  │
# │  й ще не запущена, тож новий пакет почне працювати сам після │
# │  встановлення (SUPER+Shift+R).                               │
# ╰──────────────────────────────────────────────────────────────╯

# run <ім'я процесу> <команда…> — запустити, якщо програма є і ще не працює
run() {
    name=$1; shift
    command -v "$1" >/dev/null 2>&1 || return 0
    pgrep -x "$name" >/dev/null || setsid -f "$@" >/dev/null 2>&1
}

# Музика: клавіші керують тим плеєром, що грає; другий — на паузу (~/.local/bin/music-active)
run music-active "$HOME/.local/bin/music-active"

# Робочий стіл: еквалайзер з годинником, календар, музика (сховати — % у меню SUPER)
"$HOME/.config/hypr/desk.sh"

# Сповіщення без спливних вікон: swaync — у режимі «не турбувати» (лише приймає),
# звук і історію дає віджет сповіщень (desk). swaync міг ще не запуститись — кілька спроб у фоні.
( for _ in 1 2 3 4 5 6 7 8 9 10; do swaync-client --dnd-on >/dev/null 2>&1 && break; sleep 1; done ) &

# Автояскравість — лише по кліку на кнопку у waybar (~/.local/bin/autobright once), без фонової програми

# Плавний зум щипком двох пальців у будь-якій програмі (як на Mac) — ~/.local/bin/pinch-zoom
run pinch-zoom "$HOME/.local/bin/pinch-zoom"

# Вікно запиту пароля для програм, яким потрібні права адміністратора
run hyprpolkitagent /usr/lib/hyprpolkitagent/hyprpolkitagent

# Сторож режиму живлення: прошивка при зміні профілю (Fn+Q тощо) сама ставить CPU 24/29 Вт —
# повертає наш ліміт і оновлює значок у waybar (~/.local/bin/powermode-watch, чекає подію D-Bus)
run powermode-watch "$HOME/.local/bin/powermode-watch"

# Автоблокування / притемнення / вимкнення екрана (hypr/hypridle.conf)
run hypridle hypridle

# Плашка гучності та яскравості (~/.config/swayosd/style.css)
run swayosd-server swayosd-server

# Флешки: підключати автоматично, сповіщати; значок у треї лише коли щось підключено
run udiskie udiskie --automount --notify --smart-tray

# Історія буфера обміну (SUPER+Shift+V): текст і картинки
if command -v cliphist >/dev/null 2>&1; then
    pgrep -f 'wl-paste --type text --watch cliphist'  >/dev/null || setsid -f wl-paste --type text  --watch cliphist store >/dev/null 2>&1
    pgrep -f 'wl-paste --type image --watch cliphist' >/dev/null || setsid -f wl-paste --type image --watch cliphist store >/dev/null 2>&1
fi
