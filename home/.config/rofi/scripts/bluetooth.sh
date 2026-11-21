#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  BLUETOOTH — меню в стилі SUPER (клік по значку у waybar)    │
# │  Поки меню відкрите — Bluetooth постійно шукає пристрої, і   │
# │  нові з'являються в списку самі (без кнопки «Шукати»).       │
# │  Показуються лише назви (пристрої без назви — приховані).    │
# │  Геймпади Switch 2 (міст switch2-bt) — теж у списку, 󰊴.       │
# │  значок — підключений / спарений. Enter на пристрої:           │
# │  підключений — відключити; спарений — підключити;            │
# │  новий — спарити, довіряти й підключити.                     │
# │  Працює через bluetoothctl (служба bluetooth).               │
# ╰──────────────────────────────────────────────────────────────╯
I_CONN=$'\U000f00b1'   # значок «підключено» (Nerd Font, не емодзі)
I_PAIR=$'\U000f012c'   # значок «спарено»
menu() { rofi -dmenu -i -no-custom -p "$1" -theme-str 'window { width: 520px; }' \
              -theme-str 'listview { lines: 12; }' -theme-str 'mode-switcher { enabled: false; }' "${@:2}"; }
note() { notify-send -a "Bluetooth" -t 3000 "$@"; }
bt()   { timeout 15 bluetoothctl "$@" 2>/dev/null; }
info() { bt info "$1" | grep -q "$2: yes"; }

if ! bt show | grep -q "Powered: yes"; then
    [ "$(printf '󰂯  Увімкнути Bluetooth' | menu ' Bluetooth вимкнено')" ] && { rfkill unblock bluetooth; sleep 1; bt power on >/dev/null; } && note "Bluetooth увімкнено" && exec "$0"
    exit
fi

tmp=$(mktemp -d)                 # macs — адреси в тому ж порядку, що й рядки меню
scan_pid=""; list_pid=""
cleanup() {
    [ -n "$list_pid" ] && kill "$list_pid" 2>/dev/null
    [ -n "$scan_pid" ] && kill "$scan_pid" 2>/dev/null    # зупинка bluetoothctl = кінець пошуку
    rm -rf "$tmp"
}
trap cleanup EXIT

# Значок за типом пристрою (поле Icon у bluetoothctl info)
icon() {
    case "$(bt info "$1" | sed -n 's/^\s*Icon: //p')" in
        audio-head*|audio-card) echo "󰋋" ;;   # навушники / гарнітура
        input-gaming)           echo "󰊴" ;;   # геймпад
        input-mouse)            echo "󰍽" ;;   # миша
        input-keyboard)         echo "󰌌" ;;   # клавіатура
        phone)                  echo "󰏲" ;;   # телефон
        computer)               echo "󰟀" ;;   # комп'ютер
        *)                      echo "󰂯" ;;
    esac
}

# Видає рядки меню: спершу вже відомі (підключені → спарені → інші),
# далі раз на секунду дописує нових знайдених. Назва = Alias; якщо назви немає,
# BlueZ підставляє адресу через дефіси — такі пропускаємо, доки назва не з'явиться.
# Геймпади Switch 2 веде окремий міст (switch2-bt, ~/.config/nso-gc/config.json):
# BlueZ не знає їхньої назви, тому беремо її з налаштувань мосту — «адреса назва».
sw2_list() {
    python3 -I -c 'import json,os
try:
    c=json.load(open(os.path.expanduser("~/.config/nso-gc/config.json")))
    [print(x["mac"], x.get("name") or "Switch 2") for x in c.get("controllers",[])]
except Exception: pass' 2>/dev/null
}

lister() {
    declare -A seen sw2
    while read -r m n; do [ -n "$m" ] && sw2[$m]="$n"; done < <(sw2_list)
    printf -- '-\n' >> "$tmp/macs"; echo "󰂲  Вимкнути Bluetooth"
    local first=1
    while :; do
        local con pair rows=()
        con=$(bt devices Connected); pair=$(bt devices Paired)
        while read -r _ mac name; do
            [ -z "$mac" ] || [ -n "${seen[$mac]}" ] && continue
            [ -n "${sw2[$mac]}" ] && continue          # геймпади Switch 2 — окремо нижче
            [ -z "$name" ] || [ "$name" = "${mac//:/-}" ] && continue
            local mark=" " rank=2
            if grep -q "$mac" <<<"$con"; then mark="$I_CONN"; rank=0
            elif grep -q "$mac" <<<"$pair"; then mark="$I_PAIR"; rank=1; fi
            seen[$mac]=1
            rows+=("$rank $mac $mark  $(icon "$mac")  $name")
        done < <(bt devices)
        # Геймпади Switch 2: $I_CONN — міст підключив, $I_PAIR — спарений (підключиться від будь-якої кнопки)
        if [ $first = 1 ]; then
            for mac in "${!sw2[@]}"; do
                local mark="$I_PAIR" rank=1
                if grep -q "$mac" <<<"$con"; then mark="$I_CONN"; rank=0; fi
                seen[$mac]=1
                rows+=("$rank $mac $mark  󰊴  ${sw2[$mac]}")
            done
        fi
        if [ ${#rows[@]} -gt 0 ]; then
            # на першому проході — сортування за станом; далі — у порядку знаходження
            [ $first = 1 ] && mapfile -t rows < <(printf '%s\n' "${rows[@]}" | sort -s -k1,1n)
            for r in "${rows[@]}"; do
                r=${r#* }; printf '%s\n' "${r%% *}" >> "$tmp/macs"; echo "${r#* }"
            done
        fi
        first=0
        sleep 1
    done
}

# Постійний пошук, доки меню відкрите (bluetoothctl тримає scan, поки працює)
bluetoothctl --timeout 3600 scan on >/dev/null 2>&1 &
scan_pid=$!

mkfifo "$tmp/in"
lister > "$tmp/in" &
list_pid=$!
# Без -no-custom: з ним rofi чекає кінця списку (а він «живий» — не закінчується) і не з'являється.
# -async-pre-read 0 — показати меню одразу, не чекаючи перших 25 рядків
idx=$(rofi -dmenu -i -p ' Bluetooth' -theme-str 'window { width: 520px; }' \
           -theme-str 'listview { lines: 12; }' -theme-str 'mode-switcher { enabled: false; }' -format i -async-pre-read 0 -mesg "$I_CONN підключено · $I_PAIR спарено · 󰂰 пошук пристроїв…" < "$tmp/in")
kill "$list_pid" 2>/dev/null; list_pid=""

[ -z "$idx" ] || [ "$idx" -lt 0 ] 2>/dev/null && exit   # Esc або введений текст, якого немає в списку (-1)
mac=$(sed -n "$((idx + 1))p" "$tmp/macs")
if [ "$mac" = "-" ]; then
    # rfkill block — радіо повністю знеструмлене, і стан переживає перезавантаження (systemd-rfkill)
    rfkill block bluetooth && note "Bluetooth вимкнено"; exit
fi
[ -z "$mac" ] && exit
name=$(bt info "$mac" | sed -n 's/^\s*Alias: //p')
# Геймпад Switch 2: підключенням керує міст switch2-bt, не bluetoothctl
sw2name=$(sw2_list | awk -v m="$mac" '$1 == m { $1 = ""; sub(/^ /, ""); print }')
if [ -n "$sw2name" ]; then
    if info "$mac" Connected; then
        bt disconnect "$mac" >/dev/null && note "Відключено: $sw2name" "Натисни будь-яку кнопку — підключиться знову"
    else
        note "$sw2name" "Натисни будь-яку кнопку на геймпаді — підключиться сам"
    fi
    exit
fi
# Спарений, але без збережених ключів (Bonded: no) — BlueZ відкидає його підключення
# («Rejected connection from !bonded device»): спарити наново
if info "$mac" Paired && ! info "$mac" Bonded; then
    bt remove "$mac" >/dev/null
fi
if info "$mac" Connected && info "$mac" Bonded; then
    bt disconnect "$mac" >/dev/null && note "Відключено: $name"
elif info "$mac" Paired; then
    bt connect "$mac" | grep -q "successful" && note "Підключено: $name" || note "Не вдалося підключити" "$name"
else
    note "Спарюю…" "$name"         # пошук ще йде (до виходу зі скрипта) — новий пристрій не «загубиться»
    bt pairable on >/dev/null      # без цього BlueZ спарює, але НЕ зберігає ключі → геймпади (DualShock) не підключаються
    bt pair "$mac" >/dev/null; bt trust "$mac" >/dev/null
    bt connect "$mac" | grep -q "successful" && note "Підключено: $name" || note "Не вдалося підключити" "$name — чи увімкнений на ньому режим спарювання?"
fi
