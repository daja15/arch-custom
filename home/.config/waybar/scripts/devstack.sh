#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  DEV-СТЕК — кнопка у waybar (custom/devstack)                │
# │    devstack.sh           стан для waybar (JSON)              │
# │    devstack.sh toggle    ЛКМ: запустити все / зупинити все   │
# │    devstack.sh open      ПКМ: відкрити сайт, адмінку, Adminer │
# │  Запуск: docker-db (mysql-loc, postgres, adminer) +          │
# │  PHP-сервери: сайт :8082, адмінка :8083 → відкрити в браузері.│
# │  Стан визначається за портами (ss), щоб не будити Docker.    │
# ╰──────────────────────────────────────────────────────────────╯
# Шляхи до проєкту — у ~/.config/devstack.conf (не в dotfiles):
#   COMPOSE=~/шлях/docker-compose.yml   APP=~/шлях/до/сайту
COMPOSE="$HOME/Projects/docker-db/docker-compose.yml"
APP="$HOME/Projects/site"
[ -f "$HOME/.config/devstack.conf" ] && . "$HOME/.config/devstack.conf"
LOGS="$HOME/.cache/devstack"
SITE=http://127.0.0.1:8082
ADMIN=http://127.0.0.1:8083
ADMINER=http://127.0.0.1:8080
ICON=""                              # значок Docker
SIG=15                                   # pkill -RTMIN+15 waybar — оновити значок
PHP=$(command -v php8.4 || command -v php)

up()      { ss -Hltn "sport = :$1" | grep -q .; }
refresh() { pkill -RTMIN+$SIG waybar; }
note()    { notify-send -a "Dev-стек" -t 4000 "$@"; }
wait_up() { local i; for i in $(seq 60); do up "$1" && return 0; sleep 1; done; return 1; }
open()    { xdg-open "$SITE" >/dev/null 2>&1 & sleep 0.5; xdg-open "$ADMIN" >/dev/null 2>&1 & }

start() {
    echo starting > "$LOGS/state"; refresh
    note "Запускаю…" "Docker (MySQL, Postgres, Adminer) і PHP-сервери"
    docker compose -f "$COMPOSE" up -d > "$LOGS/docker.log" 2>&1 \
        || { rm -f "$LOGS/state"; refresh; note -u critical "Docker не запустився" "$(tail -3 "$LOGS/docker.log")"; exit 1; }
    cd "$APP" || exit 1
    up 8082 || setsid -f "$PHP" -S 127.0.0.1:8082 -t frontend/web > "$LOGS/site.log" 2>&1
    up 8083 || setsid -f "$PHP" -S 127.0.0.1:8083 -t backend/web  > "$LOGS/admin.log" 2>&1
    wait_up 3306 && wait_up 8082 && wait_up 8083
    rm -f "$LOGS/state"; refresh
    if up 8082 && up 8083; then open; note "Запущено" "Сайт $SITE
Адмінка $ADMIN
Adminer $ADMINER"
    else note -u critical "Не все запустилось" "Логи: $LOGS"; fi
}

stop() {
    pkill -f -- "-S 127.0.0.1:808[23]"
    docker compose -f "$COMPOSE" stop > "$LOGS/docker.log" 2>&1
    # гасимо й сам демон (docker.socket лишається — наступний старт підніме його знову);
    # без пароля — завдяки polkit-правилу ~/.local/share/devstack/50-devstack-docker.rules
    systemctl stop --no-ask-password docker.service containerd.service >> "$LOGS/docker.log" 2>&1
    refresh; note "Зупинено" "Docker (разом із демоном) і PHP-сервери вимкнено"
}

mkdir -p "$LOGS"
case "$1" in
    toggle) [ -f "$LOGS/state" ] && exit 0                 # уже запускається
            if up 3306 || up 8082; then stop; else start; fi ;;
    open)   if up 8082; then open; xdg-open "$ADMINER" >/dev/null 2>&1 & else note "Dev-стек вимкнено" "ЛКМ по значку — запустити"; fi ;;
    *)
        if [ -f "$LOGS/state" ]; then
            printf '{"text":"%s","tooltip":"Запускається…","class":"busy"}\n' "$ICON"
        elif up 3306 || up 8082; then
            printf '{"text":"%s","tooltip":"Проєкт працює\\nСайт :8082 · Адмінка :8083 · Adminer :8080\\nЛКМ — зупинити · ПКМ — відкрити посилання","class":"on"}\n' "$ICON"
        else
            printf '{"text":"%s","tooltip":"Docker + проєкт вимкнено\\nЛКМ — запустити й відкрити сайт та адмінку","class":"off"}\n' "$ICON"
        fi ;;
esac
