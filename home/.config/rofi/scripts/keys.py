#!/usr/bin/env python3
# ╭──────────────────────────────────────────────────────────────╮
# │  ШПАРГАЛКА КЛАВІШ — SUPER+/                                  │
# │  Збирається автоматично з hypr/keyBinds.lua: розділи — з     │
# │  рядків «-- ──── НАЗВА ────», описи — з коментарів біля      │
# │  кожного bind(...). Нова клавіша з коментарем з'являється    │
# │  тут сама. Нижче — клавіші всередині програм (EXTRA).        │
# │  Пошук: просто друкуй (напр. «музика», «знімок», «Shift+P»). │
# ╰──────────────────────────────────────────────────────────────╯
import html
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.expanduser("~/.config/theme/gen"))
import theme  # noqa: E402  — кольори й шрифти з ~/.config/theme/theme.conf

KEYBINDS = os.path.expanduser("~/.config/hypr/keyBinds.lua")

# Клавіші всередині програм (їх немає в keyBinds.lua)
EXTRA = {
    "Меню SUPER — символ на початку": [
        ("(без символу)", "програми, вікна, швидкі дії; рядок-підказка каже, що зробить Enter"),
        ("@ команда", "виконати в терміналі (укр. розкладка: \")"), ("/ запит", "шукати в інтернеті (укр.: .)"),
        ("%", "швидкі дії: нічне світло, знімок, запис, піпетка, живлення…"),
        ("' назва", "файли з домашньої теки: текст, фото, відео, документи, програми"),
        ("Shift+Enter", "файл — показати у теці"), ("Ctrl+Enter", "виконати введений текст як є"),
    ],
    "Вибір знімка (після Print / SUPER+X)": [
        ("1 / R", "область"), ("2 / W", "вікно"), ("3 / L", "ласо"), ("4 / F", "весь екран"),
        ("Tab / Shift+Tab", "наступний / попередній режим"), ("E", "редагувати після знімка: так / ні"),
        ("Esc / правий клік", "скасувати"),
    ],
    "Музика — вкладка «Пошук»": [
        ("Enter", "слухати (з цієї пісні й далі за списком)"), ("порожній Enter", "моя бібліотека YouTube Music"),
        ("Ctrl+D", "скачати"), ("Ctrl+R", "радіо: схожі пісні"), ("Ctrl+A", "пісні ↔ альбоми"),
        ("Ctrl+O", "екран «Зараз грає»"), ("Tab", "вибрати кілька"), ("Alt+1 / Alt+2", "вкладки Бібліотека / Пошук"),
    ],
    "Музика — екран «Зараз грає»": [
        ("Пробіл", "пауза"), ("n / p", "наступна / попередня"), ("← →", "перемотка"), ("r", "повтор треку"),
        ("d", "скачати те, що грає"), ("9 / 0", "гучність"), ("m", "без звуку"),
        ("q", "назад (музика грає далі)"), ("s", "зупинити"),
    ],
    "micro (редактор)": [
        ("Ctrl+S / Ctrl+Q", "зберегти / вийти"), ("Ctrl+Z / Ctrl+Y", "відмінити / повторити"),
        ("Ctrl+F, Ctrl+N", "пошук, наступне"), ("Ctrl+E", "командний рядок (replace, goto, term…)"),
        ("Alt+E", "дерево файлів"), ("Alt+N", "ще один курсор на наступне таке слово"),
        ("Ctrl+/", "закоментувати"), ("Alt+↑ / Alt+↓", "перемістити рядок"), ("Ctrl+D", "дублювати рядок"),
        ("Ctrl+G", "довідка"),
    ],
}

NAMES = {   # як показувати назви клавіш
    "SUPER": "SUPER", "SHIFT": "Shift", "CTRL": "Ctrl", "CONTROL": "Ctrl", "ALT": "Alt",
    "up": "↑", "down": "↓", "left": "←", "right": "→", "period": ".", "comma": ",", "slash": "/",
    "mouse_down": "коліщатко ↓", "mouse_up": "коліщатко ↑", "mouse:272": "ліва кнопка", "mouse:273": "права кнопка",
    "Escape": "Esc", "Return": "Enter", "space": "Пробіл", "TAB": "Tab", "Tab": "Tab",
    "XF86AudioRaiseVolume": "Гучність +", "XF86AudioLowerVolume": "Гучність −", "XF86AudioMute": "Без звуку",
    "XF86AudioMicMute": "Мікрофон", "XF86MonBrightnessUp": "Яскравість +", "XF86MonBrightnessDown": "Яскравість −",
    "XF86AudioNext": "Наступний трек", "XF86AudioPrev": "Попередній трек",
    "XF86AudioPlay": "Відтворення", "XF86AudioPause": "Пауза",
}
KEYLIKE = re.compile(r"^(SUPER|Shift|Ctrl|Alt|Print|F\d+|Esc|XF86)", re.I)


def key_from_arg(arg):
    """`mod .. " + SHIFT + F"` → «SUPER+Shift+F»"""
    arg = re.sub(r"\bmod\b", '"SUPER"', arg)
    arg = re.sub(r"\.\.\s*key\b", '.. "N"', arg)
    parts = "".join(re.findall(r'"([^"]*)"', arg))
    keys = [k.strip() for k in parts.split("+") if k.strip()]
    return "+".join(NAMES.get(k, NAMES.get(k.upper(), k)) for k in keys)


def parse():
    sections, cur = [], None
    for line in open(KEYBINDS, encoding="utf-8"):
        m = re.match(r"^--\s*─+\s*(.+?)\s*─+", line)
        if m:
            cur = (m.group(1), [])
            sections.append(cur)
            continue
        m = re.match(r"^\s*(?:hl\.)?bind\((.+?),\s*(.*)$", line)
        if not m or "function bind" in line or cur is None:
            continue
        comment = line.split(" -- ", 1)[1].strip() if " -- " in line else ""
        if not comment or "SUPER_L" in m.group(1):   # службові прив'язки самого SUPER — пропустити
            continue
        if " — " in comment and KEYLIKE.match(comment):
            key, desc = comment.split(" — ", 1)
            key = key.replace(" + ", "+")
        else:
            key, desc = key_from_arg(m.group(1)), comment
        cur[1].append((key.strip(), desc.strip()))
    sections = [s for s in sections if s[1]]
    sections += list(EXTRA.items())
    return sections


def main():
    C = theme.C
    rows = []
    for title, items in parse():
        rows.append(f"<span color='{C['accent2']}'><b>{html.escape(title)}</b></span>")
        for key, desc in items:
            rows.append(f"   <span color='{C['accent']}'><b>{html.escape(key)}</b></span>"
                        f"<span color='{C['muted']}'>  ·  </span>{html.escape(desc)}")
    subprocess.run(["rofi", "-dmenu", "-i", "-markup-rows", "-no-custom",
                    "-p", "  Клавіші",                     # іконка клавіатури
                    "-mesg", "Друкуй, щоб знайти: «музика», «знімок», «Shift+P»… · Esc — закрити",
                    "-theme-str", "window { width: 820px; }",
                    "-theme-str", "listview { lines: 18; }",
                    "-theme-str", "mode-switcher { enabled: false; }",
                    "-theme-str", 'entry { placeholder: "Пошук клавіші або дії…"; }'],
                   input="\n".join(rows), text=True, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
