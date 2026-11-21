#!/bin/sh
# ╭──────────────────────────────────────────────────────────────╮
# │  Режим живлення (waybar, сигнал 17).                         │
# │    без аргументу — стан у JSON (за реальним PL1 з sysfs)     │
# │    toggle | eco | boost — перемкнути                         │
# ╰──────────────────────────────────────────────────────────────╯
PL1=/sys/class/powercap/intel-rapl-mmio:0/constraint_0_power_limit_uw
w=$(( $(cat "$PL1") / 1000000 ))
gpu=$(cat /sys/class/drm/card*/gt_max_freq_mhz 2>/dev/null | head -n1)
cur=eco; [ "$w" -ge 28 ] && cur=boost        # 28+ Вт — тільки наш буст
odd=; [ $cur = eco ] && [ "$w" -gt 10 ] && odd=1   # ні економ, ні буст — ліміт збила прошивка

case "${1:-}" in
    "")
        if [ $cur = boost ]; then   # ракета (md-rocket_launch)
            printf '{"text":"\\udb85\\udcde","tooltip":"Буст: CPU %s Вт, GPU %s МГц — клік, щоб повернути економ","class":"boost"}\n' "$w" "$gpu"
        elif [ -n "$odd" ]; then    # знак оклику: ліміт не наш
            printf '{"text":"\\udb80\\udf2a!","tooltip":"Увага: CPU %s Вт (не економ) — клік, щоб повернути економ","class":"odd"}\n' "$w"
        else                        # листок (md-leaf)
            printf '{"text":"\\udb80\\udf2a","tooltip":"Економ: CPU %s Вт, GPU %s МГц — клік, щоб увімкнути буст","class":"eco"}\n' "$w" "$gpu"
        fi
        exit 0 ;;
    toggle) { [ $cur = boost ] || [ -n "$odd" ]; } && want=eco || want=boost ;;
    eco|boost) want=$1 ;;
    *) echo "використання: powermode.sh [toggle|eco|boost]" >&2; exit 1 ;;
esac

# спершу профіль ppd: прошивка Lenovo при його зміні сама переписує PL1
# (balanced → 24 Вт, performance → 29 Вт), тож наш ліміт ставимо ПІСЛЯ
if [ $want = boost ]; then
    powerprofilesctl set performance 2>/dev/null; msg="Буст: CPU 30 Вт, GPU на максимум"
else
    powerprofilesctl set balanced 2>/dev/null;    msg="Економ: CPU 10 Вт, GPU 900 МГц"
fi
sleep 1
if ! sudo -n /usr/local/bin/power-mode "$want" >/dev/null 2>&1; then
    notify-send -a "Живлення" "Перемикач не встановлено" "sudo sh ~/.local/share/power-mode/install.sh"
    exit 1
fi
notify-send -a "Живлення" "$msg"
pkill -RTMIN+17 -x waybar
