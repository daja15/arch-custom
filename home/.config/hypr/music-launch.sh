#!/bin/sh
# ╭──────────────────────────────────────────────────────────────╮
# │  Запуск лівої панелі «Музика» (hypr/music.lua): kitty з      │
# │  двома вкладками:                                            │
# │    1. Пошук      — music-find: пошук у YouTube Music,        │
# │                    слухати / скачати                         │
# │    2. Бібліотека — kew грає скачане з ~/Music                │
# │  Якщо чогось не встановлено — сповіщення з командою          │
# │  встановлення.                                               │
# ╰──────────────────────────────────────────────────────────────╯
PAD="12 14 12 22"                    # внутрішні відступи терміналу: текст не заходить під заокруглені кути екрана
MUSIC="$HOME/Music"                  # тека бібліотеки (та сама, що в ~/.local/bin/music-find)

missing=""
for c in kew yt-dlp mpv fzf; do command -v "$c" >/dev/null 2>&1 || missing="$missing $c"; done
python3 -c 'import ytmusicapi, mutagen' 2>/dev/null || missing="$missing python-ytmusicapi python-mutagen"
if [ -n "$missing" ]; then
    notify-send -a "Музика" "Не встановлено:$missing" \
        "sudo pacman -S kew yt-dlp mpv mpv-mpris fzf python-ytmusicapi python-mutagen deno yt-dlp-ejs"
    exit 1
fi

mkdir -p "$MUSIC"
# Перший запуск kew: вказати теку бібліотеки (kew збереже її у своєму конфігу)
[ -f "$HOME/.config/kew/kewrc" ] || kew path "$MUSIC" >/dev/null 2>&1

# Сесія kitty: дві вкладки — 1) «Пошук» (YouTube Music), 2) «Бібліотека» (kew, локальна музика).
# Відкривається «Пошук». Вкладки перемикаються за назвою (music-ctl), тож порядок можна міняти.
SESSION="${XDG_RUNTIME_DIR:-/tmp}/music-panel.session"
cat > "$SESSION" <<SES
new_tab  Пошук
launch --title music-find $HOME/.local/bin/music-find
focus
new_tab  Бібліотека
launch --title kew kew
SES

# --listen-on + allow_remote_control: music-ctl перемикає вкладки (іконка пошуку, SUPER+Ctrl+F)
exec kitty --class music-panel \
    --listen-on "unix:${XDG_RUNTIME_DIR:-/tmp}/music-kitty" \
    -o allow_remote_control=socket-only \
    -o window_padding_width="$PAD" \
    -o tab_bar_min_tabs=1 \
    -o tab_bar_edge=top \
    -o tab_bar_style=separator \
    -o tab_separator="   " \
    -o 'tab_title_template=" {title} "' \
    -o 'map alt+1 goto_tab 1' \
    -o 'map alt+2 goto_tab 2' \
    -o 'map ctrl+tab next_tab' \
    -o confirm_os_window_close=0 \
    --session "$SESSION"
