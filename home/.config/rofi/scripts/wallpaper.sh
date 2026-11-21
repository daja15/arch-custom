#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  ШПАЛЕРИ — швидка заміна (SUPER+Shift+W)                     │
# │  Колекція: ~/Pictures/Wallpapers (просто кидай туди картинки)│
# │                                                              │
# │  wallpaper              — меню з мініатюрами (rofi)          │
# │  wallpaper ФАЙЛ         — поставити файл (копіюється в       │
# │                           колекцію; можна й file:///… адресу)│
# │  wallpaper next|prev    — наступні / попередні з колекції    │
# │  wallpaper random       — випадкові                          │
# │                                                              │
# │  У меню: Enter — поставити · Alt+Delete — прибрати з колекції│
# │  (у кошик) · Alt+O — відкрити теку                           │
# │  Поточні шпалери копіюються в ~/.config/hypr/wallpaper.jpg — │
# │  їх бере hyprpaper при старті системи.                       │
# ╰──────────────────────────────────────────────────────────────╯

DIR="$HOME/Pictures/Wallpapers"                  # колекція
CUR="$HOME/.config/hypr/wallpaper.jpg"           # те, що завантажить hyprpaper після перезапуску
STATE="$HOME/.cache/wallpaper/current"           # шлях до поточної картинки в колекції
THUMBS="$HOME/.cache/wallpaper/thumbs"           # мініатюри для меню
mkdir -p "$DIR" "$THUMBS"

note() { notify-send -a "Шпалери" "$@" 2>/dev/null; }

# Усі картинки колекції за абеткою
list() { find "$DIR" -maxdepth 1 -type f \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' -o -iname '*.webp' \) | sort; }

# file:///home/…%D0%97… → звичайний шлях
unurl() {
    local p="${1#file://}"
    printf '%b' "${p//%/\\x}"
}

# Поставити картинку (шлях усередині колекції)
apply() {
    local f="$1"
    # hyprpaper: підвантажити й показати на всіх моніторах (порожнє ім’я = усі)
    if pgrep -x hyprpaper >/dev/null; then
        hyprctl hyprpaper wallpaper ",$f" >/dev/null
    else
        hyprctl dispatch exec hyprpaper >/dev/null
    fi
    # Запам’ятати для наступного старту (hyprpaper.conf читає wallpaper.jpg)
    case "${f,,}" in
        *.jpg|*.jpeg) cp -f "$f" "$CUR" ;;
        *)            ffmpeg -loglevel error -y -i "$f" -q:v 2 "$CUR" ;;   # png/webp → jpg
    esac
    mkdir -p "${STATE%/*}"; printf '%s\n' "$f" > "$STATE"
}

# Додати файл у колекцію (якщо він ще не там) і повернути шлях у колекції
adopt() {
    local f; f=$(realpath -- "$1")
    case "$f" in "$DIR"/*) printf '%s' "$f"; return ;; esac
    local dst="$DIR/${f##*/}"
    if [ -e "$dst" ] && ! cmp -s "$f" "$dst"; then             # інша картинка з таким ім’ям
        dst="$DIR/$(date +%Y%m%d-%H%M%S)-${f##*/}"
    fi
    cp -n -- "$f" "$dst"
    printf '%s' "$dst"
}

# Мініатюра (робиться раз; оновлюється, якщо файл змінився)
thumb() {
    local f="$1" t="$THUMBS/$(printf '%s' "$1" | md5sum | cut -c1-16).jpg"
    if [ ! -s "$t" ] || [ "$f" -nt "$t" ]; then
        ffmpeg -loglevel error -y -i "$f" -vf "scale=320:-1" -frames:v 1 -q:v 4 "$t"
    fi
    printf '%s' "$t"
}

# Крок по колекції: +1 / -1
step() {
    mapfile -t all < <(list)
    [ ${#all[@]} -eq 0 ] && { note "Колекція порожня" "$DIR"; exit 1; }
    local cur i n=${#all[@]} idx=-1
    cur=$(cat "$STATE" 2>/dev/null)
    for i in "${!all[@]}"; do [ "${all[$i]}" = "$cur" ] && idx=$i; done
    apply "${all[$(( (idx + $1 + n) % n ))]}"
}

menu() {
    mapfile -t all < <(list)
    if [ ${#all[@]} -eq 0 ]; then
        note "Колекція порожня" "Додай картинки в $DIR"; xdg-open "$DIR" & exit 0
    fi
    local cur sel_row=0 i
    cur=$(cat "$STATE" 2>/dev/null)
    for i in "${!all[@]}"; do [ "${all[$i]}" = "$cur" ] && sel_row=$i; done
    # Рядки «назва\0icon\x1fмініатюра» — нульовий байт не можна тримати в змінній, тож друкуємо одразу в rofi
    rows() {
        local f name
        for f in "${all[@]}"; do
            name="${f##*/}"; name="${name%.*}"
            [ "$f" = "$cur" ] && name="● $name"                  # поточні позначені крапкою
            printf '%s\0icon\x1f%s\n' "$name" "$(thumb "$f")"
        done
    }
    local idx code
    idx=$(rows | rofi -dmenu -i -format i -selected-row "$sel_row" \
        -show-icons -p "󰸉  Шпалери" \
        -mesg "Enter — поставити · Alt+Delete — у кошик · Alt+O — відкрити теку" \
        -kb-custom-1 "Alt+Delete" -kb-custom-2 "Alt+o" \
        -theme-str 'window { width: 900px; }' \
        -theme-str 'listview { columns: 3; lines: 3; flow: horizontal; fixed-height: false; }' \
        -theme-str 'element { orientation: vertical; padding: 8px; }' \
        -theme-str 'element-icon { size: 150px; }' \
        -theme-str 'element-text { horizontal-align: 0.5; }' \
        -theme-str 'mode-switcher { enabled: false; }' \
        -theme-str 'entry { placeholder: "Пошук за назвою…"; }')
    code=$?
    case $code in
        0)  [ -n "$idx" ] && apply "${all[$idx]}" ;;
        10) [ -n "$idx" ] && { gio trash -- "${all[$idx]}" && note "Прибрано в кошик" "${all[$idx]##*/}"; exec "$0"; } ;;
        11) xdg-open "$DIR" & ;;
    esac
}

case "${1:-}" in
    ""|menu)    menu ;;
    next)       step 1 ;;
    prev)       step -1 ;;
    random)     mapfile -t all < <(list); [ ${#all[@]} -gt 0 ] && apply "${all[RANDOM % ${#all[@]}]}" ;;
    -h|--help)  sed -n '2,15p' "$0" | sed 's/^# //' ;;
    *)          f=$(unurl "$1")
                [ -f "$f" ] || { echo "Немає файлу: $f" >&2; note "Немає файлу" "$f"; exit 1; }
                apply "$(adopt "$f")" ;;
esac
