#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  ЗБІРКА програм на C (усі в цій теці)                        │
# │    build.sh              усі три                             │
# │    build.sh desk         лише одну: desk|visualizer|uni|pip|stars│
# │                                                              │
# │    desk        віджети столу (GTK3): desk.c — картки й запуск,│
# │                cal.c, music.c, edges.c … — по файлу на віджет │
# │                → ~/.local/bin/desk                           │
# │    visualizer  круговий еквалайзер + годинник (GTK4)         │
# │                → ~/.local/bin/visualizer                     │
# │    uni         плагін меню rofi (SUPER) → ~/.local/lib/rofi/  │
# │    pip         відео «картинка в картинці» (GTK4 + libmpv)   │
# │                → ~/.local/bin/pip                            │
# │    stars       зірки «-|-», що спалахують по екрану (GTK3)   │
# │    islands     кліки waybar лише по острівцях (GTK3-модуль)  │
# │                → ~/.local/lib/waybar-islands.so              │
# │                → ~/.local/bin/stars                          │
# │                                                              │
# │  Спільне: hyprwatch.c (чи видно віджет), тема — theme.h з    │
# │  ~/.config/theme/gen/ (немає — генерується тут же).          │
# │  Потрібно: gcc, gtk3, gtk4, gtk-layer-shell, gtk4-layer-shell,│
# │  json-glib, wayland, rofi (усе вже є в системі).             │
# ╰──────────────────────────────────────────────────────────────╯
set -e
cd "$(dirname "$0")"
THEME="$HOME/.config/theme/gen"
[ -f "$THEME/theme.h" ] || "$HOME/.config/theme/apply" --gen >/dev/null
mkdir -p "$HOME/.local/bin" "$HOME/.local/lib/rofi"
CFLAGS=(-O2 -Wall -Wextra -Wno-unused-parameter -I. -I"$THEME")

# Прапорці бібліотек для кожної програми
declare -A LIBS=(
    [desk]="gtk+-3.0 gdk-wayland-3.0 gtk-layer-shell-0 json-glib-1.0 gio-unix-2.0 wayland-client libpulse libpulse-mainloop-glib"
    [visualizer]="gtk4 gtk4-layer-shell-0 json-glib-1.0 gio-unix-2.0"
    [uni]="glib-2.0 gmodule-2.0 cairo pango rofi"
    [stars]="gtk+-3.0 gdk-wayland-3.0 gtk-layer-shell-0 json-glib-1.0 gio-unix-2.0 wayland-client pangocairo"
    [islands]="gtk+-3.0"
    [pip]="gtk4 gtk4-wayland gtk4-layer-shell-0 mpv epoxy json-glib-1.0 gio-unix-2.0"
)

build() {
    local name=$1 out src=("$1.c")
    local flags
    # shellcheck disable=SC2086  # список бібліотек має розбиватися на окремі назви
    read -ra flags <<< "$(pkg-config --cflags --libs ${LIBS[$name]})"
    case $name in
        uni) out="$HOME/.local/lib/rofi/uni.so"; flags+=(-shared -fPIC -D_DEFAULT_SOURCE) ;;
        islands) out="$HOME/.local/lib/waybar-islands.so"; flags+=(-shared -fPIC) ;;   # кліки waybar лише по острівцях
        desk) out="$HOME/.local/bin/$name"; src+=(cal.c music.c weather.c mixer.c notify.c edges.c pads.c tablet.c devconn.c battery.c bt.c sysmon.c dev.c games.c out.c wifi.c hyprwatch.c)   # віджети — окремі файли
              if pkg-config --exists vte-2.91; then          # термінал на столі — лише якщо є vte3 (sudo pacman -S vte3)
                  src+=(term.c); flags+=(-DHAVE_VTE $(pkg-config --cflags --libs vte-2.91))
              fi ;;
        *)   out="$HOME/.local/bin/$name"; src+=(hyprwatch.c) ;;
    esac
    gcc "${CFLAGS[@]}" "${src[@]}" -o "$out" "${flags[@]}" -lm
    echo "Зібрано: $out"
}

# Підказки для аналізатора коду в редакторі (clangd): де лежать заголовки бібліотек
# shellcheck disable=SC2046,SC2086
printf '%s\n' "${CFLAGS[@]}" $(pkg-config --cflags ${LIBS[desk]} ${LIBS[visualizer]} ${LIBS[uni]} | tr ' ' '\n' | sort -u) > compile_flags.txt

for name in "${@:-desk visualizer uni pip stars islands}"; do
    for n in $name; do build "$n"; done
done
