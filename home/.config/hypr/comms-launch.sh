#!/bin/sh
# ╭──────────────────────────────────────────────────────────────╮
# │  Запуск програм правої панелі «Зв'язок» (hypr/comms.lua).    │
# │  Аргумент — id програми. Кожне вікно має свій клас — за ним  │
# │  comms.lua ставить його в панель. Якщо програму не           │
# │  встановлено — сповіщення з командою встановлення.           │
# ╰──────────────────────────────────────────────────────────────╯
PAD="12 22 12 14"   # внутрішні відступи терміналу: текст не заходить під заокруглені кути екрана

need() {   # need <команда> <назва> <як встановити>
    command -v "$1" >/dev/null 2>&1 && return
    notify-send -a "Зв'язок" "$2 не встановлено" "Встанови: $3"
    exit 1
}

case "$1" in
    nchat)      # Telegram у терміналі
        exec kitty --class comms-nchat -o window_padding_width="$PAD" -e "$HOME/.config/nchat/comms-run.sh" ;;
    mail)       # Пошта Gmail + ukr.net у терміналі (aerc)
        need aerc "aerc (пошта)" "sudo pacman -S aerc"
        exec kitty --class comms-mail -o window_padding_width="$PAD" -e "$HOME/.config/aerc/comms-run.sh" ;;
    discord)    # Discord у терміналі (endcord; вхід — QR-код, пошта або токен, при першому запуску)
        need endcord "endcord (Discord)" "yay -S endcord"
        exec kitty --class comms-discord -o window_padding_width="$PAD" -e "$HOME/.config/endcord/comms-run.sh" ;;
    *)
        echo "невідома програма: $1" >&2; exit 2 ;;
esac
