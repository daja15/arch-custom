#!/usr/bin/env bash
# Відповісти на ОДИН HTTP-запит стартової сторінки Firefox і завершитись.
# Запускає systemd (firefox-start@.service) для кожного з'єднання з 127.0.0.1:47800;
# stdin/stdout — сам сокет. Віддає лише файли з цієї теки: / → index.html, /bg.jpg → фото.
DIR="$(dirname "$(readlink -f "$0")")"

IFS=' ' read -r -t 2 _ path _ || exit 0
# Дочитати заголовки запиту до порожнього рядка (інакше закриття обірве з'єднання)
while IFS= read -r -t 2 l && [ -n "${l%$'\r'}" ]; do :; done

case "${path%%\?*}" in
    /|/index.html) file="$DIR/index.html"; type="text/html; charset=utf-8" ;;
    /bg.jpg)       file="$DIR/bg.jpg";     type="image/jpeg" ;;
    *)             printf 'HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'; exit 0 ;;
esac

printf 'HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %s\r\nCache-Control: max-age=3600\r\nConnection: close\r\n\r\n' \
    "$type" "$(stat -c %s "$file")"
cat "$file"
