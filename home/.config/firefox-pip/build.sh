#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  ПЛАГІН FIREFOX «Відео поверх вікон» — збірка й встановлення │
# │    build.sh   зібрати pip.xpi (кольори — з теми), покласти   │
# │               його в профіль Firefox і зареєструвати міст    │
# │               ~/.local/bin/pip-bridge                        │
# │  Плагін не підписаний Mozilla — Firefox прийме його лише     │
# │  після firefox-autoconfig/ (один раз, з sudo — див. README   │
# │  у цій теці). Після встановлення — перезапусти Firefox.      │
# ╰──────────────────────────────────────────────────────────────╯
set -e
cd "$(dirname "$0")"
ID="pip@desk.local"

# 1. Пакет: src/ → pip.xpi, у content.css — кольори теми
python3 - <<'PY'
import os, re, sys, zipfile
sys.path.insert(0, os.path.expanduser("~/.config/theme/gen"))
from theme import C
with zipfile.ZipFile("pip.xpi", "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir("src")):
        data = open(os.path.join("src", name), encoding="utf-8").read()
        if name.endswith(".css"):
            data = re.sub(r"\{([a-z0-9-]+)\}", lambda m: C.get(m.group(1), m.group(0)), data)
        z.writestr(name, data)
print("Зібрано: pip.xpi")
PY

# 2. Міст для Firefox (і новий шлях профілю ~/.config/mozilla, і старий ~/.mozilla)
for dir in "$HOME/.config/mozilla/native-messaging-hosts" "$HOME/.mozilla/native-messaging-hosts"; do
    mkdir -p "$dir"
    cat > "$dir/pip_bridge.json" <<JSON
{ "name": "pip_bridge", "description": "Відео поверх вікон (pipctl)", "path": "$HOME/.local/bin/pip-bridge",
  "type": "stdio", "allowed_extensions": ["$ID"] }
JSON
done

# 3. У профіль(і) Firefox: extensions/<id>.xpi — Firefox підхопить при запуску
for prof in "$HOME"/.config/mozilla/firefox/*.default* "$HOME"/.mozilla/firefox/*.default*; do
    [ -d "$prof" ] || continue
    mkdir -p "$prof/extensions"
    cp pip.xpi "$prof/extensions/$ID.xpi"
    echo "У профіль: $prof"
done
