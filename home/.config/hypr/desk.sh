#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  РОБОЧИЙ СТІЛ — віджети й еквалайзер                         │
# │    desk.sh                     запустити все увімкнене       │
# │    desk.sh toggle calendar     календар: показати / сховати  │
# │    desk.sh toggle music        музика: показати / сховати    │
# │    desk.sh toggle visualizer   еквалайзер: показати / сховати│
# │    desk.sh toggle weather      погода                        │
# │    desk.sh toggle mixer        мікшер звуку                  │
# │    desk.sh toggle notify       історія сповіщень             │
# │    desk.sh toggle pads         підключені геймпади           │
# │    desk.sh toggle tablet       графічний планшет (Wacom…)    │
# │    desk.sh toggle battery      заряд бездротових пристроїв   │
# │    desk.sh toggle bluetooth    Bluetooth: пристрої, пошук    │
# │    desk.sh toggle terminal     термінал на столі (vte3)      │
# │    desk.sh toggle system       система: CPU, t°, RAM, ват    │
# │    desk.sh toggle devstack     Dev-стек: Docker + проєкт    │
# │    desk.sh toggle wifi         Wi-Fi: мережі, підключення    │
# │  (Wi-Fi, Bluetooth / Ігри, Dev-стек — висувні панелі: язички │
# │   праворуч від «Сповіщень»; toggle прибирає язичок)          │
# │    desk.sh toggle games        ігри: на ПК і останні в Steam │
# │    desk.sh toggle output       вихід звуку: вибір пристрою   │
# │    desk.sh toggle stars        зірки по екрану: увімкнути / ні│
# │    desk.sh toggle video        відео поверх вікон (pip):     │
# │                                показати / сховати            │
# │                                                              │
# │  Сховане пам'ятається (і після перезавантаження) — файли     │
# │  ~/.config/hypr/<назва>.off (calendar, deskmusic, weather,   │
# │  mixer, notify, pads, tablet, battery, bluetooth, terminal, system,  │
# │  devstack, games, wifi,                                      │
# │  visualizer, stars)                                          │
# │                                                              │
# │  Програми (код — hypr/src/, збірка — hypr/src/build.sh):     │
# │    ~/.local/bin/desk        календар, музика, погода, мікшер,│
# │                             сповіщення й гарячі краї         │
# │                             екрана (працює завжди)           │
# │    ~/.local/bin/visualizer  круговий еквалайзер + годинник   │
# │    ~/.local/bin/stars       зірки «-|-» по всьому екрану     │
# │  Позиції віджетів — ~/.cache/desk*.pos (видалити — на місце  │
# │  за замовчуванням).                                          │
# ╰──────────────────────────────────────────────────────────────╯
H="$HOME/.config/hypr"
BIN="$HOME/.local/bin"

off() { [ -f "$H/$1.off" ]; }

run() {   # run <програма> — запустити, якщо ще немає (зібрати, якщо немає)
    pgrep -x "$1" >/dev/null && return 1
    [ -x "$BIN/$1" ] || "$H/src/build.sh" "$1" >/dev/null 2>&1
    setsid -f "$BIN/$1" >/dev/null 2>&1
}

start() {
    # календар, музика й гарячі краї — одна програма; SIGHUP — перечитати, що показувати
    run desk || pkill -HUP -x desk
    if off visualizer; then pkill -x visualizer; else run visualizer; fi
    if off stars; then pkill -x stars; else run stars; fi
}

case "$1" in
    toggle)
        if [ "$2" = video ]; then                # відео поверх вікон — окрема програма (~/.local/bin/pip)
            if pgrep -x pip >/dev/null; then "$BIN/pipctl" toggle
            else notify-send -a "Відео" "Відео поверх вікон не відкрите" "Кнопка «▶ Поверх вікон» у Firefox або SUPER+Alt+O"
            fi
            exit 0
        fi
        case "$2" in
            calendar)   f=calendar ;;
            music)      f=deskmusic ;;
            visualizer) f=visualizer ;;
            stars)      f=stars ;;
            weather|mixer|notify|pads|tablet|battery|bluetooth|terminal|system|devstack|games|output|wifi) f=$2 ;;
            *) echo "desk.sh toggle calendar|music|weather|mixer|notify|pads|tablet|battery|bluetooth|terminal|system|devstack|games|output|wifi|visualizer|stars|video" >&2; exit 2 ;;
        esac
        if off "$f"; then rm -f "$H/$f.off"; else touch "$H/$f.off"; fi
        start
        [ "$f" = stars ] && { sleep 0.2; pkill -RTMIN+14 -x waybar; }   # оновити іконку зірок у waybar
        exit 0 ;;
    *) start ;;
esac
