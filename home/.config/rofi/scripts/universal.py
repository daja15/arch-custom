#!/usr/bin/env python3
# ╭──────────────────────────────────────────────────────────────╮
# │  УНІВЕРСАЛЬНЕ МЕНЮ — скрипт-режими для rofi                  │
# │  Викликається з launcher.sh. Режими (перший аргумент):       │
# │    acts  — швидкі дії (символ %): нічне світло, знімок,      │
# │            запис, піпетка, буфер, емодзі, живлення…          │
# │    files — файли з домашньої теки (символ '): текст, фото,   │
# │            відео, документи, програми. Список — готовий кеш  │
# │            (files.rofi), його просто виводить files.sh.      │
# │            Кеш оновлює таймер systemd rofi-files.timer       │
# │            (раз на 20 хв, з найнижчим пріоритетом).          │
# │    term  — виконати команду в терміналі (символ @)           │
# │    web   — шукати в інтернеті (символ /)                     │
# │    text  — Ctrl+Enter: введений текст «як є»                 │
# │    reveal— показати файл у файловому менеджері               │
# │    copy  — скопіювати результат обчислення (з плагіна uni)   │
# │    files-index — оновити кеш файлів (так його кличе таймер)  │
# ╰──────────────────────────────────────────────────────────────╯

import ast
import math
import operator
import os
import re
import shutil
import subprocess
import sys

# ───────────── НАЛАШТУВАННЯ ─────────────
HOME         = os.path.expanduser("~")
FILES_DEPTH  = 8          # глибина пошуку файлів від ~
FILES_LIMIT  = 30000      # максимум файлів у списку
FILES_SKIP   = {"node_modules", "__pycache__", "go", "target", "venv", ".venv", "site-packages",
                "build", "dist", "vendor", "bower_components", "Games", "snap", "steamapps"}   # теки, які пропускаємо (+ усі приховані)
FILES_LINKS  = {"Диск"}   # посилання на інші теки, в які теж заходимо (~/Диск → другий диск)
FILES_INDEX  = os.path.expanduser("~/.cache/rofi/files.rofi")  # кеш: готові рядки для rofi (див. files.sh)
FILES_MAXAGE = 86400      # секунд: якщо таймер не спрацював і кеш старший за добу — оновити у фоні
SEARCH_URL   = "https://duckduckgo.com/?q={}"   # пошук в інтернеті (символ /)
CONFIRM_SYS  = True       # питати підтвердження для «Вийти / Перезавантажити / Вимкнути»

RETV = int(os.environ.get("ROFI_RETV", "0"))   # 0 = список, 1 = вибрано рядок, 2 = власний текст
INFO = os.environ.get("ROFI_INFO", "")         # прихована інформація вибраного рядка


# ───────────── ДОПОМІЖНЕ ─────────────
def row(text, icon=None, info=None, extra=""):
    """Рядок для rofi: текст + іконка + прихована інформація."""
    opts = []
    if icon: opts.append(f"icon\x1f{icon}")
    if info: opts.append(f"info\x1f{info}")
    out = text
    if opts: out += "\0" + "\x1f".join(opts)
    print(out + extra)

def spawn(*cmd):
    """Запустити у фоні, повністю від'єднавши від rofi (інакше rofi чекатиме)."""
    subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)

def spawn_sh(cmd):
    spawn("sh", "-c", cmd)

def notify(title, body=""):
    if shutil.which("notify-send"):
        spawn("notify-send", "-a", "rofi", "-t", "3000", title, body)

def copy(text, notify=None):
    """Скопіювати в буфер ПІСЛЯ закриття rofi (поки rofi тримає фокус, wl-copy може не спрацювати)."""
    if os.fork() != 0:                                 # батьківський процес одразу повертається до rofi
        return
    os.setsid()
    for _ in range(100):                               # чекаємо до 2 с, поки rofi закриється
        if subprocess.run(["pgrep", "-x", "rofi"], stdout=subprocess.DEVNULL).returncode != 0:
            break
        import time; time.sleep(0.02)
    subprocess.run(["wl-copy", "--", text], stdin=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if notify:                                         # повідомлення у віджеті (зі звуком)
        subprocess.run(["notify-send", "-a", "Калькулятор", "-i", "accessories-calculator", "-t", "2000", notify],
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os._exit(0)


# ───────────── ПОСИЛАННЯ ─────────────
URL_RE = re.compile(
    r"^(https?://\S+"                               # http://… / https://…
    r"|www\.\S+"                                    # www.…
    r"|[\w-]+(\.[\w-]+)*\.[a-zA-Z]{2,}(:\d+)?(/\S*)?"  # домен.тлд[/шлях]
    r"|localhost(:\d+)?(/\S*)?)$"
)

def as_url(text):
    t = text.strip()
    if " " in t or not URL_RE.match(t):
        return None
    return t if re.match(r"^https?://", t) else "https://" + t


# ───────────── МАТЕМАТИКА (безпечно, без eval) ─────────────
OPS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
       ast.Div: operator.truediv, ast.FloorDiv: operator.floordiv, ast.Mod: operator.mod,
       ast.Pow: operator.pow, ast.USub: operator.neg, ast.UAdd: operator.pos}
FUNCS = {k: getattr(math, k) for k in ("sqrt", "sin", "cos", "tan", "asin", "acos", "atan",
                                       "log", "log10", "log2", "exp", "floor", "ceil", "factorial")}
FUNCS.update(abs=abs, round=round, pi=math.pi, e=math.e)

def _ev(n):
    if isinstance(n, ast.Constant) and isinstance(n.value, (int, float)): return n.value
    if isinstance(n, ast.BinOp) and type(n.op) in OPS:
        l, r = _ev(n.left), _ev(n.right)
        if isinstance(n.op, ast.Pow) and abs(r) > 1000: raise ValueError("завеликий степінь")
        return OPS[type(n.op)](l, r)
    if isinstance(n, ast.UnaryOp) and type(n.op) in OPS: return OPS[type(n.op)](_ev(n.operand))
    if isinstance(n, ast.Name) and n.id in FUNCS and not callable(FUNCS[n.id]): return FUNCS[n.id]
    if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and callable(FUNCS.get(n.func.id)):
        return FUNCS[n.func.id](*[_ev(a) for a in n.args])
    raise ValueError("не математика")

def calc(text):
    """Повертає рядок-результат або None, якщо це не вираз."""
    t = text.strip().replace("^", "**").replace("×", "*").replace("÷", "/").replace(",", ".")
    t = re.sub(r"(\d+(?:\.\d+)?)\s*%", r"(\1/100)", t)        # 15% → 0.15
    if not re.search(r"\d", t) or not re.search(r"[-+*/%()a-z]", t):
        return None                                            # просто число або слово — не рахуємо
    try:
        v = _ev(ast.parse(t, mode="eval").body)
    except Exception:
        return None
    if isinstance(v, float):
        if v.is_integer() and abs(v) < 1e15: v = int(v)
        else: v = float(f"{v:.10g}")
    return str(v)


# ───────────── ОБРОБКА ВВЕДЕНОГО ТЕКСТУ ─────────────
def handle_text(text):
    """Ctrl+Enter у меню: посилання → браузер, вираз → буфер, команда → запуск, інше → пошук."""
    text = text.strip()
    if not text:
        return
    url = as_url(text)
    if url:                                            # 1) посилання → браузер
        spawn("xdg-open", url)
        return
    res = calc(text)
    if res is not None:                                # 2) математика → буфер обміну
        copy(res)
        notify(f"= {res}", f"{text}\nСкопійовано в буфер обміну")
        return
    first = text.split()[0]
    if shutil.which(os.path.expanduser(first)):        # 3) команда → запуск
        spawn_sh(text)
        return
    from urllib.parse import quote_plus                # 4) інше → пошук в інтернеті
    spawn("xdg-open", SEARCH_URL.format(quote_plus(text)))


# ───────────── ШВИДКІ ДІЇ (символ %) ─────────────
HYPR  = "~/.config/hypr/"
ROFIS = "~/.config/rofi/scripts/"

def running(*names):
    """Які з програм зараз запущені — один прохід по /proc (замість окремого pgrep на кожну)."""
    want, found = set(names), set()
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                with open(f"/proc/{pid}/comm") as f:
                    c = f.read().strip()
            except OSError:
                continue
            if c in want:
                found.add(c)
    return found


def boost_on():
    """Буст = PL1 від 28 Вт (див. ~/.config/waybar/scripts/powermode.sh)."""
    try:
        with open("/sys/class/powercap/intel-rapl-mmio:0/constraint_0_power_limit_uw") as f:
            return int(f.read()) >= 28000000
    except OSError:
        return False


def acts():
    """(підпис, іконка, команда, потребує підтвердження). Список будується щоразу — стан актуальний."""
    run = running("hyprsunset", "visualizer", "desk", "pip", "stars")
    night_on, vis_on, desk_on = "hyprsunset" in run, "visualizer" in run, "desk" in run
    cal_on = desk_on and not os.path.exists(os.path.expanduser(HYPR + "calendar.off"))
    mus_on = desk_on and not os.path.exists(os.path.expanduser(HYPR + "deskmusic.off"))
    on = lambda name: desk_on and not os.path.exists(os.path.expanduser(HYPR + name + ".off"))
    video = "pip" in run
    return ([("Відео поверх вікон: показати / сховати", "video-display", HYPR + "desk.sh toggle video", False)]
            if video else []) + [
        ("Нічне світло: " + ("вимкнути" if night_on else "увімкнути"),
                                 "weather-clear-night",   HYPR + "nightlight.sh",              False),
        ("Живлення: " + ("повернути економ" if boost_on() else "сильний буст"),
                                 "battery",               "~/.config/waybar/scripts/powermode.sh toggle", False),
        ("Еквалайзер на фоні: " + ("вимкнути" if vis_on else "увімкнути"),
                                 "audio-x-generic",       HYPR + "desk.sh toggle visualizer", False),
        ("Зірки на екрані: " + ("вимкнути" if "stars" in run else "увімкнути"),
                                 "starred",               HYPR + "desk.sh toggle stars",      False),
        ("Календар на столі: " + ("сховати" if cal_on else "показати"),
                                 "x-office-calendar",     HYPR + "desk.sh toggle calendar",   False),
        ("Музика на столі: " + ("сховати" if mus_on else "показати"),
                                 "audio-x-generic",       HYPR + "desk.sh toggle music",      False),
        ("Погода на столі: " + ("сховати" if on("weather") else "показати"),
                                 "weather-few-clouds",    HYPR + "desk.sh toggle weather",    False),
        ("Мікшер звуку на столі: " + ("сховати" if on("mixer") else "показати"),
                                 "audio-volume-high",     HYPR + "desk.sh toggle mixer",      False),
        ("Сповіщення на столі: " + ("сховати" if on("notify") else "показати"),
                                 "preferences-system-notifications", HYPR + "desk.sh toggle notify", False),
        ("Геймпади на столі: " + ("сховати" if on("pads") else "показати"),
                                 "input-gaming",          HYPR + "desk.sh toggle pads",       False),
        ("Планшет на столі: " + ("сховати" if on("tablet") else "показати"),
                                 "input-tablet",          HYPR + "desk.sh toggle tablet",     False),
        ("Заряд пристроїв на столі: " + ("сховати" if on("battery") else "показати"),
                                 "battery",               HYPR + "desk.sh toggle battery",    False),
        ("Wi-Fi на столі: " + ("сховати" if on("wifi") else "показати"),
                                 "network-wireless",      HYPR + "desk.sh toggle wifi",       False),
        ("Bluetooth на столі: " + ("сховати" if on("bluetooth") else "показати"),
                                 "bluetooth",             HYPR + "desk.sh toggle bluetooth",  False),
        ("Термінал на столі: " + ("сховати" if on("terminal") else "показати"),
                                 "utilities-terminal",    HYPR + "desk.sh toggle terminal",   False),
        ("Система на столі: " + ("сховати" if on("system") else "показати"),
                                 "utilities-system-monitor", HYPR + "desk.sh toggle system",  False),
        ("Dev-стек на столі: " + ("сховати" if on("devstack") else "показати"),
                                 "applications-development", HYPR + "desk.sh toggle devstack", False),
        ("Вихід звуку на столі: " + ("сховати" if on("output") else "показати"),
                                 "audio-speakers",        HYPR + "desk.sh toggle output",     False),
        ("Ігри на столі: " + ("сховати" if on("games") else "показати"),
                                 "applications-games",    HYPR + "desk.sh toggle games",      False),
        ("Знімок екрана (вибрати область)", "applets-screenshooter", HYPR + "snip.py",       False),
        ("Знімок усього екрана",  "camera-photo",          HYPR + "snip.py --mode screen --now", False),
        ("Запис відео екрана",    "media-record",          HYPR + "snip.py --video",           False),
        ("Піпетка: колір з екрана", "color-select",        ROFIS + "colorpick.sh",             False),
        ("Історія буфера обміну", "edit-paste",            ROFIS + "clipboard.sh",             False),
        ("Емодзі",                "face-smile",            ROFIS + "emoji.sh",                 False),
        ("Тема: вибрати",         "preferences-desktop-theme", "~/.config/theme/theme",     False),
        ("Тема: темна ↔ світла",  "preferences-desktop-theme", "~/.config/theme/theme toggle", False),
        ("Шпалери: вибрати",      "preferences-desktop-wallpaper", ROFIS + "wallpaper.sh",   False),
        ("Шпалери: наступні",     "preferences-desktop-wallpaper", ROFIS + "wallpaper.sh next", False),
        ("Шпаргалка клавіш",      "preferences-desktop-keyboard", ROFIS + "keys.py",          False),
        ("Музика: показати / сховати", "audio-x-generic",  "hyprctl eval 'MusicToggle()'",     False),
        ("Перезапустити waybar",  "view-refresh",          "hyprctl eval 'WaybarRestart()'",   False),
        ("Перечитати налаштування Hyprland", "view-refresh", "hyprctl reload",                 False),
        ("Заблокувати екран",     "system-lock-screen",    "hyprlock",                         False),
        ("Сон",                   "system-suspend",        "hyprlock & sleep 0.5; systemctl suspend", False),
        ("Вийти з сесії",         "system-log-out",        "hyprctl dispatch 'hl.dsp.exit()'", True),
        ("Перезавантажити",       "system-reboot",         "systemctl reboot",                 True),
        ("Вимкнути комп'ютер",    "system-shutdown",       "systemctl poweroff",               True),
    ]

AFTER_ROFI = "while pgrep -x rofi >/dev/null; do sleep 0.05; done; "   # спершу дочекатися, поки меню закриється

def acts_mode():
    items = acts()
    if RETV == 0:
        for i, (label, icon, _, _) in enumerate(items):
            row(label, icon=icon, info=str(i))
        return
    try:
        label, _, cmd, confirm = items[int(INFO)]
    except (ValueError, IndexError):
        return
    if confirm and CONFIRM_SYS:
        # Підтвердження окремим вікном — після того, як це меню закриється
        q = label.replace("'", "")
        spawn_sh(
            AFTER_ROFI +
            f"a=$(printf 'Так, {q.lower()}\\nСкасувати' | rofi -dmenu -i -no-custom -p '{q}?' "
            "-theme-str 'window { width: 360px; }' -theme-str 'listview { lines: 2; }' "
            "-theme-str 'mode-switcher { enabled: false; }' -theme-str 'inputbar { children: [ prompt ]; }'); "
            f"[ \"${{a#Так}}\" != \"$a\" ] && {cmd}"
        )
    else:
        spawn_sh(AFTER_ROFI + cmd)


# ───────────── ТЕРМІНАЛ (символ @) І ІНТЕРНЕТ (символ /) ─────────────
KITTY = ("kitty", "--single-instance", "--instance-group=term")   # спільний процес з іншими терміналами

def term(cmd):
    """Виконати команду в новому вікні kitty; вікно лишається відкритим, поки не натиснеш Enter."""
    cmd = cmd.strip()
    if not cmd:
        spawn(*KITTY)
        return
    script = (f"printf '\\033[1;36m$ %s\\033[0m\\n' {shq(cmd)}; {cmd}; "
              "printf '\\n\\033[2m[код виходу %s · Enter — закрити]\\033[0m' $?; read -r _")
    os.environ["NO_FETCH"] = "1"                      # без логотипа neofetch (див. ~/.zshrc)
    spawn(*KITTY, "--title", f"$ {cmd}", "zsh", "-ic", script)

def shq(t):
    return "'" + t.replace("'", "'\\''") + "'"

def web(text):
    from urllib.parse import quote_plus
    text = text.strip()
    spawn("xdg-open", SEARCH_URL.format(quote_plus(text)) if text else "https://duckduckgo.com")


# ───────────── ФАЙЛИ (символ ') ─────────────
# Лише ці типи (за розширенням). Інші файли (кеш, бінарні дрібниці) у список не потрапляють.
KINDS = {
    "text":  ("text-x-generic", "txt md markdown rst org log csv tsv json jsonc yaml yml toml ini conf cfg "
                                "xml html htm css scss js mjs ts tsx jsx py sh bash zsh fish lua c h cpp hpp "
                                "rs go java kt rb php sql tex nix rasi"),
    "image": ("image-x-generic", "jpg jpeg png gif webp bmp tif tiff svg avif heic heif jxl ico qoi "
                                 "raw dng cr2 cr3 nef arw orf rw2 raf psd xcf kra"),
    "video": ("video-x-generic", "mp4 mkv webm avi mov m4v mpg mpeg wmv flv 3gp ogv"),
    "doc":   ("x-office-document", "pdf odt ods odp odg doc docx xls xlsx ppt pptx rtf epub djvu fb2 mobi"),
    "app":   ("application-x-executable", "appimage run exe msi deb rpm jar flatpakref"),
}
EXT_KIND = {e: k for k, (_, exts) in KINDS.items() for e in exts.split()}
EXT_ICON = {"pdf": "application-pdf", "xls": "x-office-spreadsheet", "xlsx": "x-office-spreadsheet",
            "ods": "x-office-spreadsheet", "csv": "x-office-spreadsheet", "ppt": "x-office-presentation",
            "pptx": "x-office-presentation", "odp": "x-office-presentation", "md": "text-x-markdown",
            "sh": "text-x-script", "py": "text-x-python"}

def file_kind(path, name):
    ext = name.rsplit(".", 1)[-1].lower() if "." in name else ""
    if ext in EXT_KIND:
        return EXT_KIND[ext]
    if not ext and os.access(path, os.X_OK):          # виконуваний файл без розширення — програма / скрипт
        return "app"
    return None

def build_index():
    """Обійти домашню теку й записати кеш: «шлях<TAB>тип» на рядок."""
    out, count = [], 0
    base_depth = HOME.rstrip("/").count("/")
    for root, dirs, files in os.walk(HOME, followlinks=False):
        depth = root.count("/") - base_depth
        keep = []
        for d in sorted(dirs):
            if d.startswith(".") or d in FILES_SKIP:
                continue
            full = os.path.join(root, d)
            if os.path.islink(full) and not (root == HOME and d in FILES_LINKS):
                continue                              # чужі посилання не чіпаємо (можуть вести по колу)
            keep.append(d)
        dirs[:] = keep if depth < FILES_DEPTH else []
        # os.walk не заходить у посилання — ~/Диск обходимо окремо
        if root == HOME:
            for d in FILES_LINKS:
                if os.path.isdir(os.path.join(HOME, d)) and d in dirs:
                    dirs.remove(d)
        for f in sorted(files):
            if f.startswith("."):
                continue
            full = os.path.join(root, f)
            k = file_kind(full, f)
            if k:
                out.append(f"{full}\t{k}")
                count += 1
                if count >= FILES_LIMIT:
                    break
    for d in FILES_LINKS:                             # ~/Диск — та сама логіка, але з шляхом через ~/Диск
        top = os.path.join(HOME, d)
        if not os.path.isdir(top):
            continue
        for root, dirs, files in os.walk(top, followlinks=False):
            depth = root.count("/") - base_depth
            dirs[:] = [x for x in sorted(dirs) if not x.startswith(".") and x not in FILES_SKIP
                       and not os.path.islink(os.path.join(root, x))] if depth < FILES_DEPTH else []
            for f in sorted(files):
                if f.startswith("."):
                    continue
                full = os.path.join(root, f)
                k = file_kind(full, f)
                if k and count < FILES_LIMIT:
                    out.append(f"{full}\t{k}")
                    count += 1
    rows = [file_row(ln) for ln in out]
    os.makedirs(os.path.dirname(FILES_INDEX), exist_ok=True)
    tmp = FILES_INDEX + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write("".join(rows))
    os.replace(tmp, FILES_INDEX)                      # атомарно: rofi ніколи не прочитає половину
    return rows

def file_row(ln):
    """«шлях<TAB>тип» → готовий рядок rofi: «назва   ·  ~/тека» + іконка + шлях у info."""
    path, _, k = ln.partition("\t")
    name = os.path.basename(path)
    d = os.path.dirname(path)
    rel = "~" + d[len(HOME):] if d.startswith(HOME) else d
    ext = name.rsplit(".", 1)[-1].lower() if "." in name else ""
    return f"{name}   ·  {rel}\0icon\x1f{EXT_ICON.get(ext, KINDS[k][0])}\x1finfo\x1f{path}\n"

def files_mode():
    """Запасний шлях (зазвичай список виводить files.sh простим cat)."""
    if RETV != 0:
        if INFO:
            spawn("xdg-open", INFO)                   # відкрити у програмі за замовчуванням
        return
    import time
    try:
        with open(FILES_INDEX, encoding="utf-8") as fh:
            sys.stdout.write(fh.read())
        if time.time() - os.path.getmtime(FILES_INDEX) > FILES_MAXAGE:
            spawn(sys.executable, os.path.abspath(__file__), "files-index")   # таймер не працює — оновити у фоні
    except OSError:
        sys.stdout.write("".join(build_index()))       # першого разу — зібрати одразу


# ───────────── ПОКАЗАТИ ФАЙЛ У ТЕЦІ ─────────────
def reveal(path):
    path = os.path.expanduser(path)
    if os.path.isdir(path):                            # тека → просто відкрити
        spawn("xdg-open", path)
    elif shutil.which("dolphin"):                      # файл → dolphin з виділеним файлом
        spawn("dolphin", "--select", path)
    else:                                              # інші менеджери — через D-Bus FileManager1
        spawn("gdbus", "call", "--session", "--dest", "org.freedesktop.FileManager1",
              "--object-path", "/org/freedesktop/FileManager1",
              "--method", "org.freedesktop.FileManager1.ShowItems",
              f"['file://{path}']", "")


# ───────────── ТОЧКА ВХОДУ ─────────────
if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode == "acts":    acts_mode()
    elif mode == "files": files_mode()
    elif mode == "files-index": build_index()          # оновити кеш файлів (у фоні)
    elif mode == "term":  term(sys.argv[2] if len(sys.argv) > 2 else "")
    elif mode == "web":   web(sys.argv[2] if len(sys.argv) > 2 else "")
    elif mode == "text":                               # з плагіна uni: введений текст без збігів
        handle_text(sys.argv[2] if len(sys.argv) > 2 else "")
    elif mode == "reveal":                             # з плагіна uni: показати файл у теці
        reveal(sys.argv[2])
    elif mode == "copy":                               # з плагіна uni: скопіювати результат обчислення
        copy(sys.argv[2], notify="Скопійовано: " + sys.argv[2])

