#!/bin/sh
# ╭──────────────────────────────────────────────────────────────╮
# │  Пункт меню «Зв'язок» (waybar/comms.jsonc): значок + назва,  │
# │  як рядок списку на столі (ui-row): значок Nerd Font Mono    │
# │  17pt, сірий (відкрита програма — акцентний), проміжок,      │
# │  назва 10pt. Аргументи: id значок назва. Значок — JSON-кодом │
# │  (\\uXXXX), щоб символ не загубився.                          │
# │  Підсвічується (клас active), коли програма зараз відкрита — │
# │  стан пише hypr/comms.lua.                                   │
# ╰──────────────────────────────────────────────────────────────╯
id=$1 icon=$2 name=$3
. "$HOME/.config/theme/gen/theme.sh"                      # кольори теми: TH_…
cur=$(cat "${XDG_RUNTIME_DIR:-/tmp}/comms-active" 2>/dev/null)
ICON="font_family='JetBrainsMono Nerd Font Mono' size='17pt' rise='-3pt'"
if [ "$cur" = "$id" ]; then
    cls="active" color="$TH_ACCENT" nm="<b>$name</b>"
else
    cls="" color="$TH_MUTED" nm="$name"
fi
text="<span $ICON color='$color'>$icon</span><span size='8pt'>  </span>$nm"
printf '{"text":"%s","tooltip":"%s — відкрити / сховати","class":"%s"}\n' "$text" "$name" "$cls"
