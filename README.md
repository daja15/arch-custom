# Dotfiles — Hyprland «Тиха ніч»

Arch Linux + Hyprland 0.57 (конфіг на Lua). Гарно, але з мінімальним навантаженням на відеокарту.
Усі налаштування прокоментовані українською.

## Встановлення на новий комп'ютер

```sh
sudo pacman -S --needed git
git clone <адреса-цього-репозиторію> ~/dotfiles
~/dotfiles/install.sh
```

Потім вийди з сесії й зайди в Hyprland. **SUPER+/** — шпаргалка всіх клавіш.

Параметри `install.sh`:

| Параметр | Що робить |
|---|---|
| *(без параметрів)* | усе: пакети, AUR, zsh, налаштування, плагіни, служби |
| `--configs-only` | лише розкласти налаштування (пакети вже стоять) |
| `--no-packages` | без pacman / AUR |
| `--no-aur` | без AUR (nchat, tdlib) |
| `--no-zsh` | без oh-my-zsh / powerlevel10k |

Старі файли, які замінюються, зберігаються в `~/.dotfiles-backup-<дата>`.
Повторний запуск безпечний.

## Зберегти свої зміни

```sh
~/dotfiles/save.sh                 # зібрати налаштування з системи + коміт
~/dotfiles/save.sh "що змінив"     # з описом
git -C ~/dotfiles push             # відправити на GitHub (якщо підключено)
```

Що саме зберігається — у `files.txt` (рядки з `!` — виключення).

## Що всередині

| Що | Де | Клавіші |
|---|---|---|
| Hyprland: вікна, столи, анімації, правила | `.config/hypr/*.lua` | SUPER+/ — усі |
| Waybar: острівці, з'являється на SUPER / вгорі екрана | `.config/waybar` | SUPER, SUPER+R |
| Універсальне меню (rofi + плагін `uni`) | `.config/rofi` | SUPER+R |
| Панель «Музика»: kew + пошук і скачування з YouTube Music | `hypr/music.lua`, `.local/bin/music-*` | SUPER+A |
| Панель «Зв'язок»: nchat (Telegram) + aerc (пошта) | `hypr/comms.lua` | SUPER+M |
| Знімки й запис екрана: область / вікно / ласо / екран, редактор satty | `hypr/snip.py` | Print, SUPER+X, SUPER+Alt+X |
| Шпаргалка, буфер обміну, піпетка, емодзі | `.config/rofi/scripts` | SUPER+/, SUPER+Shift+V, SUPER+Shift+C, SUPER+. |
| Автоблокування, плашка гучності, флешки, polkit | `hypr/services.sh`, `hypridle.conf` | — |
| Термінал kitty + zsh (підсвічування, підказки) | `.config/kitty`, `.zshrc` | — |
| Редактор micro: тема, markdown, дерево файлів | `.config/micro` | Alt+E — дерево |
| Робочий стіл: еквалайзер з годинником, календар, музика | `hypr/src/` (C), `hypr/desk.sh` | `%` у меню — показати / сховати |
| Нічне світло | `hypr/nightlight.sh` | іконка у waybar |

## Тема — один файл на все

Кольори, шрифти, заокруглення й відступи **всіх** програм задає один файл:
`~/.config/theme/theme.conf`. Змінив → `~/.config/theme/apply`.

`apply` генерує з нього `~/.config/theme/gen/` — ту саму тему у форматі кожної програми
(CSS для waybar / сповіщень / віджетів, rasi для rofi, Lua для Hyprland, заголовок для
програм на C, модуль Python, `theme.sh` для shell, kitty, satty, схема micro із
`templates/`), перезбирає програми на C і оновлює все, що вже працює.
`gen/` у репозиторій не потрапляє — інсталятор генерує його сам.

## Чого тут немає (навмисно)

Паролі, сесії й ключі в репозиторій **не потрапляють**:

- пошта: `~/.config/aerc/accounts.conf` і паролі в `~/.local/share/comms/mail` → на новому комп'ютері: `comms-mail-setup`;
- Telegram: профілі nchat → nchat сам попросить увійти (SUPER+M);
- `~/.config/tg-contacts/api.conf` (ключі Telegram API) → `tg-contacts` спитає при першому запуску;
- бібліотека музики `~/Music`, знімки, записи.
