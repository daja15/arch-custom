#!/bin/sh
# Відкриття вкладень з nchat (Ctrl+V).
# Відео, аудіо, гіфки й фото — у VLC, який стає в праву панель поряд з чатом
# (hypr/comms.lua: CommsMedia). Попередній такий VLC закривається, щоб не плодити вікна.
# Усе інше (pdf, документи, посилання) — програмою за замовчуванням.
f="$1"
[ -e "$f" ] || exec xdg-open "$f"                       # не файл (посилання)
command -v vlc >/dev/null 2>&1 || exec xdg-open "$f"    # VLC немає — як звичайно

PIDFILE="${XDG_RUNTIME_DIR:-/tmp}/comms-vlc.pid"
mime=$(file -b --mime-type "$f")

case "$mime" in
  image/gif)         opts="--loop" ;;                   # гіфки — по колу
  video/*|audio/*)   opts="" ;;                         # відео, голосові, музика
  image/*)           opts="--image-duration=-1" ;;      # фото — показувати, доки не закриєш
  *)                 exec xdg-open "$f" ;;
esac

# Закрити попередній VLC з панелі
[ -r "$PIDFILE" ] && kill "$(cat "$PIDFILE")" 2>/dev/null

# --no-one-instance: окреме вікно, навіть якщо в налаштуваннях VLC увімкнено «один екземпляр»
# --no-qt-video-autoresize: VLC не розтягує вікно під розмір відео (розмір задає панель)
vlc --no-one-instance --no-video-title-show --no-qt-video-autoresize $opts "$f" >/dev/null 2>&1 &
pid=$!
echo "$pid" > "$PIDFILE"
hyprctl eval "CommsMedia($pid)" >/dev/null
