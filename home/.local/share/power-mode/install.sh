#!/bin/sh
# Одноразове встановлення: sudo sh ~/.local/share/power-mode/install.sh
set -e
D=/home/jijau/.local/share/power-mode
visudo -cf "$D/sudoers"
install -o root -g root -m 755 "$D/power-mode" /usr/local/bin/power-mode
install -o root -g root -m 440 "$D/sudoers" /etc/sudoers.d/power-mode
# 10 Вт при завантаженні тепер застосовується ПІСЛЯ thermald
install -o root -g root -m 644 /home/jijau/.local/share/power-limit/power-limit-10w.service /etc/systemd/system/power-limit-10w.service
# сон пристроїв у простої (ASPM, runtime PM) — при завантаженні й після сну
install -o root -g root -m 644 /home/jijau/.local/share/power-limit/runtime-pm.service /etc/systemd/system/runtime-pm.service
systemctl daemon-reload
systemctl enable --now runtime-pm.service
# прибрати старий помічник Mario Kart, якщо лишився
rm -f /usr/local/bin/kart-power /etc/sudoers.d/kart-power
/usr/local/bin/power-mode eco
echo "готово"
