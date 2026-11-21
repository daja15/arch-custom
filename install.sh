#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  INSTALL — поставити все на чистий Arch Linux                │
# │      git clone <адреса> ~/dotfiles && ~/dotfiles/install.sh  │
# │                                                              │
# │  Що робить (кожен крок можна пропустити, див. --help):       │
# │   1. пакети з репозиторіїв Arch (pacman)                     │
# │   2. yay + пакети з AUR (nchat, tdlib)                       │
# │   3. oh-my-zsh + powerlevel10k                               │
# │   4. налаштування → домашня тека (старі — у резервну копію)  │
# │   5. плагін rofi, плагін micro, ярлики, теки, служби         │
# │  Запускати від звичайного користувача (sudo спитає сам).     │
# │  Повторний запуск безпечний: встановлене пропускається.      │
# ╰──────────────────────────────────────────────────────────────╯
set -euo pipefail
REPO="$(cd "$(dirname "$0")" && pwd)"
SRC="$REPO/home"
BACKUP="$HOME/.dotfiles-backup-$(date +%Y-%m-%d_%H-%M-%S)"

# ───────────── ПАКЕТИ ─────────────
PACMAN_PKGS=(
    # Hyprland і його екосистема
    hyprland hyprlock hypridle hyprpaper hyprpicker hyprpolkitagent hyprsunset hyprcursor
    # Екран входу в систему (вибір сесії Hyprland)
    sddm
    xdg-desktop-portal-hyprland xdg-desktop-portal-gtk qt5-wayland qt6-wayland xorg-xwayland
    # Панелі, меню, сповіщення, плашка гучності
    waybar rofi rofi-emoji swaync swayosd
    # Термінал, оболонка, редактор
    kitty zsh zsh-autosuggestions zsh-syntax-highlighting micro fzf btop git base-devel curl
    # Перевірка коду в micro
    shellcheck python-pyflakes yamllint
    # Знімки й запис екрана, буфер обміну
    grim satty wf-recorder wl-clipboard cliphist
    # Звук, яскравість, живлення, флешки, мережа, bluetooth
    pipewire pipewire-pulse pipewire-alsa wireplumber gst-plugin-pipewire
    brightnessctl playerctl power-profiles-daemon udiskie networkmanager bluez bluez-utils
    # Музика: kew + пошук і скачування з YouTube Music
    kew cava mpv mpv-mpris intel-media-driver yt-dlp yt-dlp-ejs deno ffmpeg python-ytmusicapi python-mutagen
    # Помічники на Python (гарячі краї, вибір знімка): GTK3 + layer-shell
    python-gobject python-cairo gtk3 gtk-layer-shell gtk4 gtk4-layer-shell json-glib libnotify vte3
    # Бібліотеки для програм на C (віджети: мікшер — libpulse, погода — curl, відео — mpv)
    libpulse
    # Іконки в меню rofi (тема breeze-dark) + пошта в HTML (aerc → w3m)
    breeze-icons w3m
    # Програми
    firefox dolphin vlc vlc-plugin-ffmpeg aerc swayimg
    # Оновлення в waybar
    pacman-contrib
    # Шрифти (+ fonttools — зібрати «JetBrainsMono Fast»)
    ttf-jetbrains-mono-nerd noto-fonts noto-fonts-emoji noto-fonts-cjk ttf-dejavu ttf-liberation python-fonttools
)
AUR_PKGS=(
    nchat telegram-tdlib                # Telegram у терміналі + бібліотека для tg-contacts
    endcord                             # Discord у терміналі (панель «Зв'язок»)
    neofetch                            # логотип системи при відкритті термінала (.zshrc)
    figcraft photocraft vectorcraft     # графічні програми
)

# ───────────── ПАРАМЕТРИ ─────────────
DO_PKGS=1 DO_AUR=1 DO_ZSH=1 DO_FILES=1 DO_EXTRA=1
for a in "$@"; do
    case $a in
        --no-packages) DO_PKGS=0; DO_AUR=0 ;;
        --no-aur)      DO_AUR=0 ;;
        --no-zsh)      DO_ZSH=0 ;;
        --configs-only) DO_PKGS=0; DO_AUR=0; DO_ZSH=0; DO_EXTRA=0 ;;
        -h|--help)
            sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
            echo "Параметри: --no-packages  --no-aur  --no-zsh  --configs-only"
            exit 0 ;;
        *) echo "Невідомий параметр: $a (див. --help)"; exit 1 ;;
    esac
done

step() { printf '\n\033[1;36m▶ %s\033[0m\n' "$*"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; }

[[ $EUID -eq 0 ]] && { echo "Запусти від звичайного користувача, не root (sudo спитає сам)."; exit 1; }
command -v pacman >/dev/null || { echo "Потрібен Arch Linux (pacman)."; exit 1; }

# ───────────── 1. ПАКЕТИ (pacman) ─────────────
if (( DO_PKGS )); then
    step "Пакети з репозиторіїв Arch"
    sudo pacman -Syu --needed --noconfirm "${PACMAN_PKGS[@]}"
    ok "встановлено"
fi

# ───────────── 2. AUR (yay) ─────────────
if (( DO_AUR )); then
    step "AUR: ${AUR_PKGS[*]}"
    if ! command -v yay >/dev/null; then
        tmp=$(mktemp -d)
        git clone --depth 1 https://aur.archlinux.org/yay-bin.git "$tmp/yay-bin"
        (cd "$tmp/yay-bin" && makepkg -si --noconfirm)
        rm -rf "$tmp"
        ok "yay встановлено"
    fi
    yay -S --needed --noconfirm "${AUR_PKGS[@]}"
    ok "встановлено"
fi

# ───────────── 2б. ВЛАСНІ ПАКЕТИ (pkg/) ─────────────
# libinput-free-scroll — прокрутка тачпада в будь-якому напрямку, як на MacBook (замінює libinput)
if (( DO_PKGS )) && ! pacman -Q libinput-free-scroll >/dev/null 2>&1; then
    step "libinput з вільною прокруткою тачпада"
    (cd "$REPO/pkg/libinput-free-scroll" && makepkg -si --noconfirm --needed) \
        && ok "встановлено" || warn "не вдалося — лишається звичайний libinput"
fi

# ───────────── 3. ZSH: oh-my-zsh + powerlevel10k ─────────────
if (( DO_ZSH )); then
    step "oh-my-zsh + powerlevel10k"
    [[ -d $HOME/.oh-my-zsh ]] || git clone --depth 1 https://github.com/ohmyzsh/ohmyzsh.git "$HOME/.oh-my-zsh"
    P10K="$HOME/.oh-my-zsh/custom/themes/powerlevel10k"
    [[ -d $P10K ]] || git clone --depth 1 https://github.com/romkatv/powerlevel10k.git "$P10K"
    if [[ $(getent passwd "$USER" | cut -d: -f7) != */zsh ]]; then
        chsh -s "$(command -v zsh)" && ok "zsh — оболонка за замовчуванням" || warn "не вдалося змінити оболонку (chsh -s /usr/bin/zsh)"
    fi
    ok "готово"
fi

# ───────────── 3б. CLAUDE CODE (у scratchpad SUPER+S при старті) ─────────────
if (( DO_ZSH )) && ! command -v claude >/dev/null && [[ ! -x $HOME/.local/bin/claude ]]; then
    step "Claude Code"
    curl -fsSL https://claude.ai/install.sh | bash >/dev/null && ok "встановлено (увійти: запусти claude)" || warn "Claude Code не встановився"
fi

# ───────────── 4. НАЛАШТУВАННЯ ─────────────
if (( DO_FILES )); then
    step "Налаштування → $HOME  (старі файли → $BACKUP)"
    n=0
    while IFS= read -r -d '' f; do
        rel=${f#"$SRC/"}
        if [[ -e $HOME/$rel || -L $HOME/$rel ]] && ! cmp -s "$f" "$HOME/$rel"; then
            mkdir -p "$BACKUP/$(dirname "$rel")"
            cp -a "$HOME/$rel" "$BACKUP/$rel"
        fi
        mkdir -p "$HOME/$(dirname "$rel")"
        cp -a "$f" "$HOME/$rel"
        n=$((n + 1))
    done < <(find "$SRC" \( -type f -o -type l \) -print0)
    # Шляхи з домашньою текою автора → твоя домашня тека
    if [[ $HOME != /home/jijau ]]; then
        grep -rIl '/home/jijau' "$SRC" 2>/dev/null | while read -r f; do
            sed -i "s|/home/jijau|$HOME|g" "$HOME/${f#"$SRC/"}"
        done
    fi
    ok "$n файлів"
    [[ -d $BACKUP ]] && ok "резервна копія старих: $BACKUP"
    # Тема: theme.conf → ~/.config/theme/gen/ (на неї посилаються kitty, Hyprland, waybar, rofi…)
    "$HOME/.config/theme/apply" --gen >/dev/null && ok "тему згенеровано (~/.config/theme/gen)"
fi

# ───────────── 5. ДОДАТКОВО ─────────────
if (( DO_EXTRA )); then
    step "Плагіни, ярлики, теки, служби"
    chmod +x "$HOME"/.local/bin/{music-find,music-ctl,music-active,pipctl,pip-bridge,tg-contacts,comms-mail-setup,fast-font-build} \
             "$HOME"/.config/hypr/*.sh "$HOME"/.config/hypr/*.py "$HOME"/.config/rofi/scripts/* \
             "$HOME"/.config/waybar/scripts/* 2>/dev/null || true

    # Другий і третій waybar під іншими іменами (сигнали основної панелі їх не чіпають)
    ln -sf /usr/bin/waybar "$HOME/.local/bin/waybar-comms"
    ln -sf /usr/bin/waybar "$HOME/.local/bin/waybar-music"
    ln -sf "$HOME/.config/rofi/scripts/wallpaper.sh" "$HOME/.local/bin/wallpaper"   # команда wallpaper (шпалери)
    ln -sf "$HOME/.config/theme/theme" "$HOME/.local/bin/theme"                      # команда theme (перемикач тем)
    ok "waybar-comms / waybar-music / wallpaper / theme"
    # Українська мова системи (підписи дат, календар, погода)
    locale -a 2>/dev/null | grep -qi '^uk_UA.utf8' || warn "немає локалі uk_UA.UTF-8: розкоментуй її в /etc/locale.gen, sudo locale-gen, і LANG=uk_UA.UTF-8 у /etc/locale.conf"


    # Програми на C: віджети, еквалайзер, плагін меню rofi (тема вже згенерована на кроці 4)
    if bash "$HOME/.config/hypr/src/build.sh" >/dev/null; then ok "програми на C зібрано (desk, visualizer, pip, плагін rofi)"; else warn "програми на C не зібрались (потрібні gtk3, gtk4, gtk-layer-shell, gtk4-layer-shell, json-glib, rofi)"; fi

    # Плагін Firefox «Відео поверх вікон»: дозвіл непідписаних плагінів (sudo) + профіль Firefox, якщо ще нема
    FA="$HOME/.config/firefox-pip/firefox-autoconfig"
    if [[ -d /usr/lib/firefox ]]; then
        sudo install -Dm644 "$FA/autoconfig.js" /usr/lib/firefox/defaults/pref/autoconfig.js \
            && sudo install -m644 "$FA/mozilla.cfg" /usr/lib/firefox/mozilla.cfg \
            && ok "Firefox: дозволено непідписані плагіни" || warn "Firefox: autoconfig не скопіювався"
    fi
    if ! compgen -G "$HOME/.config/mozilla/firefox/*.default*" >/dev/null && ! compgen -G "$HOME/.mozilla/firefox/*.default*" >/dev/null; then
        timeout 8 firefox --headless about:blank >/dev/null 2>&1 || true     # перший запуск створює профіль
    fi
    bash "$HOME/.config/firefox-pip/build.sh" >/dev/null 2>&1 && ok "плагін Firefox покладено в профіль" || warn "плагін Firefox не зібрався"
    chmod +x "$HOME/.local/bin/pip-bridge" 2>/dev/null || true

    # Плагін micro: дерево файлів (Alt+E)
    micro -plugin install filemanager >/dev/null 2>&1 && ok "micro: filemanager" || warn "micro: filemanager не встановився"

    # Шрифт «JetBrainsMono Fast»: початок слова жирніший + лігатури коду
    if "$HOME/.local/bin/fast-font-build" >/dev/null 2>&1; then ok "шрифт JetBrainsMono Fast"; else warn "шрифт не зібрався (потрібні python-fonttools і ttf-jetbrains-mono-nerd)"; fi
    # Той самий шрифт для програм Qt / KDE (dolphin) і GNOME / GTK
    if command -v kwriteconfig6 >/dev/null; then
        for k in font menuFont toolBarFont fixed; do
            kwriteconfig6 --file kdeglobals --group General --key $k "JetBrainsMono Fast,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1"
        done
    fi
    gsettings set org.gnome.desktop.interface font-name 'JetBrainsMono Fast 10' 2>/dev/null || true

    # Теки
    mkdir -p "$HOME/Music/youtube" "$HOME/Pictures/Screenshots" "$HOME/Pictures/Wallpapers" "$HOME/Videos/Записи"
    [[ -f $HOME/.config/kew/kewrc ]] || kew path "$HOME/Music" >/dev/null 2>&1 || true
    ok "теки: Music, Pictures/Screenshots, Videos/Записи"

    # Ярлики й програми за замовчуванням
    update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

    # Системні служби
    for svc in NetworkManager bluetooth power-profiles-daemon sddm; do
        sudo systemctl enable --now "$svc" >/dev/null 2>&1 && ok "служба $svc" || warn "служба $svc не запустилась"
    done

    # Кеш файлів для меню rofi (символ ') — оновлюється таймером раз на 20 хв
    systemctl --user daemon-reload 2>/dev/null || true
    systemctl --user enable --now rofi-files.timer >/dev/null 2>&1 && ok "таймер кешу файлів rofi" || warn "таймер rofi-files не запустився"
    # Стартова сторінка Firefox (http://127.0.0.1:47800, лише сокет — у спокої 0 процесів)
    systemctl --user enable --now firefox-start.socket >/dev/null 2>&1 && ok "стартова сторінка Firefox" || warn "firefox-start.socket не запустився"

    # Заокруглені кути екрана (необов'язково, через nix)
    if command -v nix >/dev/null && [[ ! -x $HOME/.nix-profile/bin/wlr-screen-corners ]]; then
        nix profile install github:Aphyra0/wlr-screen-corners >/dev/null 2>&1 \
            && ok "wlr-screen-corners (кути екрана)" || warn "wlr-screen-corners не встановлено (nix)"
    fi
fi

step "Готово!"
cat <<'EOF'
  • Вийди з сесії й зайди в Hyprland (або перезавантаж комп'ютер).
  • SUPER+/  — шпаргалка всіх клавіш.
  • Шпалери: SUPER+Shift+W (колекція ~/Pictures/Wallpapers)
  • Тема:    SUPER+Shift+T (меню) · SUPER+Ctrl+T (темна ↔ світла)
  • Пошта:   comms-mail-setup     · Telegram / Discord: SUPER+M (самі попросять увійти)
  • Свої зміни зберігай:  ~/dotfiles/save.sh
EOF
