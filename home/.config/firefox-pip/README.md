# Плагін Firefox «Відео поверх вікон»

Кнопка на панелі Firefox, пункт у меню правого кліку («Відкрити поверх вікон»)
і кнопки на самому відео (з'являються при наведенні). Відео йде в маленький плеєр поверх усіх
вікон (`~/.local/bin/pip`) — через міст `~/.local/bin/pip-bridge`.

## Встановлення

1. Один раз (sudo) — дозволити Firefox непідписані плагіни:

       sudo install -m644 ~/.config/firefox-pip/firefox-autoconfig/autoconfig.js /usr/lib/firefox/defaults/pref/
       sudo install -m644 ~/.config/firefox-pip/firefox-autoconfig/mozilla.cfg /usr/lib/firefox/

2. Зібрати й покласти плагін у профіль: `~/.config/firefox-pip/build.sh`
3. Перезапустити Firefox.

## Будова
- `src/` — сам плагін (manifest, фон, кнопки на відео; кольори CSS — з теми);
- `build.sh` — пакує `pip.xpi`, кладе в профіль, реєструє міст;
- `firefox-autoconfig/` — дозвіл непідписаних плагінів (у `/usr/lib/firefox`).

## Сайти зі схованим відео (Kodik та інші вбудовані плеєри)
yt-dlp їх не розбирає. Плагін запам'ятовує потік `.m3u8` / `.mpd`, який вкладка завантажила сама,
і віддає плеєру його разом з Referer, Origin і User-Agent Firefox. Тому **спершу запусти відео на
сторінці** (хоч на секунду), а тоді тисни «▶ Поверх вікон».
