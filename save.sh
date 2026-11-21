#!/usr/bin/env bash
# ╭──────────────────────────────────────────────────────────────╮
# │  SAVE — зібрати поточні налаштування з системи в репозиторій │
# │  і зробити коміт. Запускати після будь-яких змін:            │
# │      ~/dotfiles/save.sh            (або ./save.sh "опис")     │
# │  Список файлів — files.txt. Паролі й сесії туди не входять.  │
# ╰──────────────────────────────────────────────────────────────╯
set -euo pipefail
REPO="$(cd "$(dirname "$0")" && pwd)"
DEST="$REPO/home"

mapfile -t ITEMS    < <(grep -vE '^\s*(#|$)' "$REPO/files.txt" | grep -v '^!')
mapfile -t EXCLUDES < <(grep '^!' "$REPO/files.txt" | sed 's/^!//')

excluded() {   # excluded <шлях відносно $HOME>
    local p=$1 e
    for e in "${EXCLUDES[@]}"; do
        # shellcheck disable=SC2053  # $e — шаблон (напр. *.pass), порівняння з шаблоном навмисне
        [[ $p == $e || $p == $e/* || $(basename "$p") == $e || $p == */$e || $p == */$e/* ]] && return 0
    done
    return 1
}

rm -rf "$DEST"
mkdir -p "$DEST"
for item in "${ITEMS[@]}"; do
    [[ -e "$HOME/$item" ]] || { echo "  пропущено (немає): $item"; continue; }
    while IFS= read -r -d '' f; do
        rel=${f#"$HOME/"}
        excluded "$rel" && continue
        mkdir -p "$DEST/$(dirname "$rel")"
        cp -a "$f" "$DEST/$rel"
    done < <(find "$HOME/$item" \( -type f -o -type l \) -print0)
done

cd "$REPO"
git add -A
if git diff --cached --quiet; then
    echo "Змін немає."
else
    git commit -q -m "${1:-Оновлення налаштувань $(date '+%Y-%m-%d %H:%M')}"
    echo "Збережено: $(git log -1 --format='%h  %s')"
fi
