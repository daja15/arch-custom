#!/bin/sh
# ╭──────────────────────────────────────────────────────────────╮
# │  Кількість доступних оновлень (pacman + AUR). Перевірка раз  │
# │  на годину (interval у config.jsonc), без оновлень — тихий   │
# │  значок ✓. Клік — оновити в терміналі.                       │
# ╰──────────────────────────────────────────────────────────────╯
command -v checkupdates >/dev/null || { echo '{"text":""}'; exit 0; }   # потрібен pacman-contrib
repo=$(checkupdates 2>/dev/null | wc -l)
aur=$(yay -Qua 2>/dev/null | wc -l)
n=$((repo + aur))
if [ "$n" -gt 0 ]; then
    printf '{"text":"\\uf0ed  %s","tooltip":"Оновлення: %s з репозиторіїв, %s з AUR — клік, щоб оновити","class":"available"}\n' "$n" "$repo" "$aur"
else
    # оновлень немає — тихий значок «усе оновлено» (клік — все одно перевірити й оновити)
    printf '{"text":"\\uf00c","tooltip":"Система оновлена — клік, щоб перевірити ще раз","class":"uptodate"}\n'
fi
