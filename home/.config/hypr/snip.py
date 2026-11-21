#!/usr/bin/env python3
# ╭──────────────────────────────────────────────────────────────╮
# │  ЗНІМОК ЕКРАНА — область / вікно / ласо / весь екран         │
# │  Екран «заморожується» (знімок grim), поверх — вибір:        │
# │    1 R  область (тягни прямокутник)                          │
# │    2 W  вікно (наведи й клацни)                              │
# │    3 L  ласо (обведи будь-яку форму)                         │
# │    4 F  весь екран (клацни або Enter)                        │
# │    Tab  наступний режим (Shift+Tab — попередній)             │
# │    E    редагувати після знімка: увімк / вимк                │
# │    V    Знімок ↔ Відео (одне вікно для обох)                 │
# │    A    звук у відео: так / ні (звук системи, не мікрофон)   │
# │    Ctrl+C  зняти вибране лише в буфер обміну (без файлу      │
# │            і без редактора)                                  │
# │    Esc  скасувати                                            │
# │  Результат: ~/Pictures/Screenshots + буфер обміну, далі —    │
# │  редактор satty (малювання, текст, стрілки, розмиття…).      │
# │  Аргументи: --mode rect|window|lasso|screen                  │
# │             --no-edit   лише зберегти й скопіювати           │
# │             --now       без вибору (з --mode screen|window:  │
# │                         весь екран / активне вікно)          │
# │             --video     вибрати область і почати ЗАПИС;      │
# │                         якщо запис іде — зупинити            │
# │  Запис: wf-recorder → ~/Videos/Записи, індикатор у waybar.   │
# │  Під час запису Print / SUPER+X теж зупиняють його.          │
# │  Клавіші — hypr/keyBinds.lua (Print …).                      │
# ╰──────────────────────────────────────────────────────────────╯
import argparse
import fcntl
import json
import math
import os
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.expanduser("~/.config/theme/gen"))
import theme  # noqa: E402  — кольори й шрифти з ~/.config/theme/theme.conf

RUN = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
OUT_DIR = os.path.expanduser("~/Pictures/Screenshots")      # куди зберігати знімки
VID_DIR = os.path.expanduser("~/Videos/Записи")             # куди зберігати записи екрана
REC_PID = os.path.join(RUN, "snip-rec.pid")                 # запис іде: pid wf-recorder
REC_FILE = os.path.join(RUN, "snip-rec.path")               # … і куди пишеться
NAME = "Знімок %Y-%m-%d %H-%M-%S.png"                       # назва файлу (strftime)

ACCENT = theme.rgb("accent")                                  # рамка вибору — колір теми
DIM = 0.45                                                    # затемнення поза вибором

# Іконки Nerd Font — кодами (символи напряму губляться при записі файлів)
I_RECT, I_WIN, I_LASSO, I_SCREEN, I_EDIT = "\U000f0489", "\U000f05af", "\U000f0f03", "\U000f0379", "\U000f03eb"
I_VIDEO = "\U000f05a0"                                       # відеокамера
I_PHOTO = "\U000f0100"                                       # фотоапарат
I_AUDIO = "\U000f057e"                                       # гучномовець
I_SHOT  = "\U000f0104"                                       # діафрагма — «зробити знімок»


def sh(*cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def hypr_json(what):
    return json.loads(sh("hyprctl", what, "-j").stdout or "null")


def notify(title, body="", icon=None):
    cmd = ["notify-send", "-a", "Знімок"]
    if icon:
        cmd += ["-i", icon]
    subprocess.Popen(cmd + [title, body], stderr=subprocess.DEVNULL)


def out_path():
    os.makedirs(OUT_DIR, exist_ok=True)
    return os.path.join(OUT_DIR, time.strftime(NAME))


def finish(path, edit):
    """Скопіювати в буфер обміну; відкрити редактор або сповістити."""
    with open(path, "rb") as f:
        subprocess.run(["wl-copy", "--type", "image/png"], stdin=f)
    if edit and shutil.which("satty"):
        # Редактор: малювання, текст, стрілки, прямокутники, розмиття, маркер, обрізка.
        # Ctrl+S — зберегти (у той самий файл), Ctrl+C — копіювати, Esc — закрити.
        subprocess.Popen(["satty", "--filename", path, "--output-filename", path,
                          "--copy-command", "wl-copy", "--early-exit"],
                         start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        notify("Знімок збережено й скопійовано", path +
               ("" if not edit else "\nДля редагування встанови: sudo pacman -S satty"), icon=path)


def focused_monitor():
    for m in hypr_json("monitors") or []:
        if m.get("focused"):
            return m
    return (hypr_json("monitors") or [None])[0]


# ───────────── ЗАПИС ЕКРАНА (wf-recorder) ─────────────
def rec_pid():
    """pid запису, що йде, або None."""
    try:
        pid = int(open(REC_PID).read())
        os.kill(pid, 0)
        return pid
    except (OSError, ValueError):
        return None


def waybar_rec_signal():
    subprocess.run(["pkill", "-RTMIN+11", "-x", "waybar"])   # оновити індикатор запису


def rec_start(geom=None, output=None, audio=False):
    """geom — «x,y wxh» у глобальних координатах; output — назва монітора (весь екран)."""
    if not shutil.which("wf-recorder"):
        notify("wf-recorder не встановлено", "sudo pacman -S wf-recorder")
        return
    os.makedirs(VID_DIR, exist_ok=True)
    path = os.path.join(VID_DIR, time.strftime("Запис %Y-%m-%d %H-%M-%S.mp4"))
    where = ["-g", geom] if geom else ["-o", output]
    if audio:                                                 # звук системи = «монітор» поточного виходу
        sink = sh("pactl", "get-default-sink").stdout.strip()
        if sink:
            where.append(f"--audio={sink}.monitor")
    # Апаратне кодування (VAAPI, Intel) — майже не вантажить процесор; якщо не вийшло — звичайне
    for codec in (["-c", "h264_vaapi", "-d", "/dev/dri/renderD128"], []):
        p = subprocess.Popen(["wf-recorder", *where, *codec, "-f", path],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)
        time.sleep(1.0)
        if p.poll() is None:
            break
    else:
        notify("Не вдалося почати запис")
        return
    open(REC_PID, "w").write(str(p.pid))
    open(REC_FILE, "w").write(path)
    waybar_rec_signal()
    notify("● Запис почався", "Зупинити: ще раз та сама клавіша, Print або клік по ● у waybar")


def rec_stop():
    pid = rec_pid()
    if not pid:
        return False
    os.kill(pid, signal.SIGINT)                               # wf-recorder коректно дописує файл
    for _ in range(50):
        try:
            os.kill(pid, 0)
            time.sleep(0.1)
        except OSError:
            break
    path = open(REC_FILE).read().strip() if os.path.exists(REC_FILE) else ""
    for f in (REC_PID, REC_FILE):
        try:
            os.unlink(f)
        except OSError:
            pass
    waybar_rec_signal()
    notify("Запис збережено", path)
    return True


# ───────────── БЕЗ ВИБОРУ (--now) ─────────────
def instant(mode, edit):
    path = out_path()
    if mode == "window":
        w = hypr_json("activewindow")
        if not w or not w.get("size"):
            notify("Немає активного вікна")
            return
        (x, y), (ww, hh) = w["at"], w["size"]
        r = sh("grim", "-g", f"{x},{y} {ww}x{hh}", path)
    else:
        m = focused_monitor()
        r = sh("grim", "-o", m["name"], path)
    if r.returncode == 0:
        finish(path, edit)
    else:
        notify("Не вдалося зробити знімок", r.stderr.strip())


# ───────────── ВИБІР (оверлей поверх замороженого екрана) ─────────────
def select(mode, edit, record=False):
    import cairo
    import gi
    gi.require_version("Gdk", "3.0")                         # явно GTK 3 (у системі є й GTK 4)
    gi.require_version("Gtk", "3.0")
    gi.require_version("GtkLayerShell", "0.1")
    from gi.repository import Gdk, Gtk, GtkLayerShell

    mon = focused_monitor()
    frozen = os.path.join(RUN, "snip-frozen.png")
    if sh("grim", "-o", mon["name"], frozen).returncode != 0:
        notify("Не вдалося зробити знімок")
        return
    img = cairo.ImageSurface.create_from_png(frozen)
    MW, MH = round(mon["width"] / mon["scale"]), round(mon["height"] / mon["scale"])   # логічний розмір
    S = img.get_width() / MW                                   # пікселів знімка на логічний піксель

    # Вікна, видимі на цьому моніторі: спершу спецстіл, плаваючі, нещодавно активні
    vis = {mon["activeWorkspace"].get("name"), (mon.get("specialWorkspace") or {}).get("name")} - {None, ""}
    wins = []
    for c in hypr_json("clients") or []:
        ws = c.get("workspace") or {}
        shown = c.get("visible", ws.get("name") in vis) and ws.get("name") in vis
        if shown and c.get("mapped", True) and not c.get("hidden"):
            x, y = c["at"][0] - mon["x"], c["at"][1] - mon["y"]
            special = ws.get("type") == "special"
            wins.append(((not special, not c["floating"], c.get("focusHistoryID", 99)),
                         (x, y, c["size"][0], c["size"][1])))
    wins = [r for _, r in sorted(wins)]

    css = Gtk.CssProvider()
    css.load_from_data((theme.CSS_IMPORT + """
    .bar { background: @surface; border: 1px solid @line; border-radius: 12px; padding: 4px; margin-top: 16px; }
    .bar button { background: transparent; border: none; box-shadow: none; border-radius: 8px;
                  color: @text; padding: 6px 12px; min-height: 0; }
    .bar button:hover    { background: @overlay; }
    .bar button.on       { background: @accent;  color: @bg; }
    .bar button.edit.on  { background: @accent2; color: @bg; }
    .bar button.video.on { background: @red;     color: @bg; }
    .bar button.photo.on { background: @accent;  color: @bg; }
    .bar button.audio.on { background: @green;   color: @bg; }
    .bar button.shot     { background: @accent;  color: @bg; font-weight: bold; margin-left: 4px; }
    .bar button.shot:hover { background: @accent-soft; }
    .bar label.hint      { color: @muted; padding: 0 10px; }
    .bar .sep            { background: @line; min-width: 1px; margin: 4px 6px; }
    * { font-size: 13px; transition: none; }
    """).encode())
    Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    st = {"mode": mode, "edit": edit, "record": record, "audio": False, "drag": None, "pts": [], "hover": None, "result": None,
          "sel": None,      # готовий вибір (область / ласо), який ще можна рухати й тягнути за маркери
          "adjust": None,   # що тягнемо зараз: (маркер, звідки почали, вибір на початку)
          "copy": False}    # Ctrl+C — лише скопіювати в буфер (без файлу й редактора)
    HANDLE = 6              # пів-розміру маркера (px)

    win = Gtk.Window()
    GtkLayerShell.init_for_window(win)
    GtkLayerShell.set_namespace(win, "snip")
    GtkLayerShell.set_layer(win, GtkLayerShell.Layer.OVERLAY)
    for e in (GtkLayerShell.Edge.TOP, GtkLayerShell.Edge.BOTTOM, GtkLayerShell.Edge.LEFT, GtkLayerShell.Edge.RIGHT):
        GtkLayerShell.set_anchor(win, e, True)
    GtkLayerShell.set_exclusive_zone(win, -1)
    GtkLayerShell.set_keyboard_mode(win, GtkLayerShell.KeyboardMode.EXCLUSIVE)
    disp = Gdk.Display.get_default()
    for i in range(disp.get_n_monitors()):                    # той монітор, де фокус
        g = disp.get_monitor(i).get_geometry()
        if g.x == mon["x"] and g.y == mon["y"]:
            GtkLayerShell.set_monitor(win, disp.get_monitor(i))

    area = Gtk.DrawingArea()
    area.add_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.BUTTON_RELEASE_MASK |
                    Gdk.EventMask.POINTER_MOTION_MASK)
    overlay = Gtk.Overlay()
    overlay.add(area)

    # ── панель режимів угорі
    bar = Gtk.Box(spacing=2)
    bar.get_style_context().add_class("bar")
    bar.set_halign(Gtk.Align.CENTER)
    bar.set_valign(Gtk.Align.START)

    def sep():
        b = Gtk.Box()
        b.get_style_context().add_class("sep")
        bar.add(b)

    # ── що робимо: знімок чи відео (V)
    pb = Gtk.Button(label=f"{I_PHOTO}  Знімок")
    pb.get_style_context().add_class("photo")
    pb.set_tooltip_text("Знімок екрана (клавіша V — перемкнути)")
    pb.connect("clicked", lambda *_: set_record(False))
    bar.add(pb)
    vb = Gtk.Button(label=f"{I_VIDEO}  Відео")
    vb.get_style_context().add_class("video")
    vb.set_tooltip_text("Запис відео (клавіша V — перемкнути)")
    vb.connect("clicked", lambda *_: set_record(True))
    bar.add(vb)
    sep()

    # ── що саме: область / вікно / ласо / екран (1–4, Tab)
    buttons = {}
    for key, icon, label, hk in (("rect", I_RECT, "Область", "1"), ("window", I_WIN, "Вікно", "2"),
                                 ("lasso", I_LASSO, "Ласо", "3"), ("screen", I_SCREEN, "Екран", "4")):
        b = Gtk.Button(label=f"{icon}  {label}")
        b.set_tooltip_text(f"Клавіша {hk}")
        b.connect("clicked", lambda _b, k=key: set_mode(k))
        bar.add(b)
        buttons[key] = b
    sep()

    # ── опції: для знімка — редагувати (E), для відео — звук (A)
    eb = Gtk.Button(label=f"{I_EDIT}  Редагувати")
    eb.get_style_context().add_class("edit")
    eb.set_tooltip_text("Відкрити редактор після знімка (клавіша E)")
    eb.connect("clicked", lambda *_: toggle_edit())
    bar.add(eb)
    ab = Gtk.Button(label=f"{I_AUDIO}  Звук")
    ab.get_style_context().add_class("audio")
    ab.set_tooltip_text("Записувати звук системи (клавіша A)")
    ab.connect("clicked", lambda *_: toggle_audio())
    bar.add(ab)
    sb = Gtk.Button(label=f"{I_SHOT}  Зробити знімок")
    sb.get_style_context().add_class("shot")
    sb.set_tooltip_text("Зняти вибране (Enter)")
    sb.connect("clicked", lambda *_: done())
    bar.add(sb)
    hint = Gtk.Label(label="V знімок/відео · Tab область · Enter знімок · Ctrl+C копіювати · Esc")
    hint.get_style_context().add_class("hint")
    bar.add(hint)
    overlay.add_overlay(bar)
    win.add(overlay)

    def refresh_buttons():
        for k, b in buttons.items():
            (b.get_style_context().add_class if k == st["mode"] else b.get_style_context().remove_class)("on")
        (eb.get_style_context().add_class if st["edit"] else eb.get_style_context().remove_class)("on")
        (ab.get_style_context().add_class if st["audio"] else ab.get_style_context().remove_class)("on")
        (vb.get_style_context().add_class if st["record"] else vb.get_style_context().remove_class)("on")
        (pb.get_style_context().add_class if not st["record"] else pb.get_style_context().remove_class)("on")
        eb.set_visible(not st["record"])                       # знімок — «Редагувати»
        ab.set_visible(st["record"])                           # відео — «Звук»
        sb.set_visible(st["sel"] is not None)                  # є вибір — можна знімати
        sb.set_label(f"{I_VIDEO if st['record'] else I_SHOT}  " + ("Почати запис" if st["record"] else "Зробити знімок"))

    def set_mode(k):
        st["mode"], st["drag"], st["pts"], st["hover"], st["sel"], st["adjust"] = k, None, [], None, None, None
        refresh_buttons()
        area.queue_draw()

    def set_record(on):
        st["record"] = on
        refresh_buttons()

    # ── налаштування вибору (як у фоторедакторі): маркери, перенесення, масштаб
    def bbox(shape):
        if shape[0] == "rect":
            return shape[1:]
        xs, ys = [p[0] for p in shape[1]], [p[1] for p in shape[1]]
        return (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))

    def handles(shape):
        """Маркери: назва → (x, y). n/s/e/w — сторони, кути — їх поєднання."""
        x, y, w, h = bbox(shape)
        return {"nw": (x, y), "n": (x + w / 2, y), "ne": (x + w, y), "e": (x + w, y + h / 2),
                "se": (x + w, y + h), "s": (x + w / 2, y + h), "sw": (x, y + h), "w": (x, y + h / 2)}

    def hit(x, y):
        """Що під курсором: маркер, «move» (усередині вибору) або None."""
        if not st["sel"]:
            return None
        for name, (hx, hy) in handles(st["sel"]).items():
            if abs(x - hx) <= HANDLE + 3 and abs(y - hy) <= HANDLE + 3:
                return name
        bx, by, bw, bh = bbox(st["sel"])
        return "move" if bx <= x <= bx + bw and by <= y <= by + bh else None

    def reshape(orig, handle, dx, dy):
        """Новий вибір: тягнемо маркер на (dx, dy) або переносимо весь (move)."""
        x, y, w, h = bbox(orig)
        if handle == "move":
            nx, ny, nw, nh = x + dx, y + dy, w, h
        else:
            l, t, r, b = x, y, x + w, y + h
            if "w" in handle: l += dx
            if "e" in handle: r += dx
            if "n" in handle: t += dy
            if "s" in handle: b += dy
            nx, ny, nw, nh = min(l, r), min(t, b), max(abs(r - l), 3), max(abs(b - t), 3)
        if orig[0] == "rect":
            return ("rect", nx, ny, nw, nh)
        sx, sy = nw / w if w else 1, nh / h if h else 1           # ласо: та сама форма в новій рамці
        return ("poly", [(nx + (px - x) * sx, ny + (py - y) * sy) for px, py in orig[1]])

    CURSORS = {"nw": "nw-resize", "n": "n-resize", "ne": "ne-resize", "e": "e-resize", "se": "se-resize",
               "s": "s-resize", "sw": "sw-resize", "w": "w-resize", "move": "move", None: "crosshair"}

    def set_cursor(name):
        gw = area.get_window()
        if gw:
            gw.set_cursor(Gdk.Cursor.new_from_name(Gdk.Display.get_default(), CURSORS.get(name, "crosshair")))

    def toggle_audio():
        st["audio"] = not st["audio"]
        refresh_buttons()

    def toggle_edit():
        st["edit"] = not st["edit"]
        refresh_buttons()

    def window_at(x, y):
        for (wx, wy, ww, wh) in wins:
            if wx <= x < wx + ww and wy <= y < wy + wh:
                return (wx, wy, ww, wh)

    def current_shape():
        """Вибір: ('rect', x, y, w, h) або ('poly', [(x, y)…]) у логічних пікселях."""
        if st["sel"] and not st["drag"] and not (st["mode"] == "lasso" and st["pts"]):
            return st["sel"]
        if st["mode"] == "rect" and st["drag"]:
            (x0, y0), (x1, y1) = st["drag"]
            return ("rect", min(x0, x1), min(y0, y1), abs(x1 - x0), abs(y1 - y0))
        if st["mode"] == "window" and st["hover"]:
            return ("rect",) + st["hover"]
        if st["mode"] == "lasso" and len(st["pts"]) > 1:
            return ("poly", st["pts"])
        if st["mode"] == "screen":
            return ("rect", 0, 0, MW, MH)
        return None

    def path_of(cr, shape):
        if shape[0] == "rect":
            cr.rectangle(*shape[1:])
        else:
            cr.move_to(*shape[1][0])
            for p in shape[1][1:]:
                cr.line_to(*p)
            cr.close_path()

    def on_draw(_w, cr):
        cr.save()
        cr.scale(1 / S, 1 / S)
        cr.set_source_surface(img, 0, 0)
        cr.paint()
        cr.restore()
        shape = current_shape()
        cr.set_fill_rule(cairo.FILL_RULE_EVEN_ODD)           # затемнити все, крім вибору
        cr.rectangle(0, 0, MW, MH)
        if shape:
            path_of(cr, shape)
        cr.set_source_rgba(0, 0, 0, DIM)
        cr.fill()
        if shape:
            path_of(cr, shape)
            cr.set_source_rgb(*ACCENT)
            cr.set_line_width(2)
            cr.stroke()
            if st["sel"] is shape:                            # маркери: тягнути / переносити
                if shape[0] == "poly":                        # у ласо — пунктирна рамка навколо
                    cr.set_dash([4, 4])
                    cr.set_line_width(1)
                    cr.rectangle(*bbox(shape))
                    cr.stroke()
                    cr.set_dash([])
                for hx, hy in handles(shape).values():
                    cr.rectangle(hx - HANDLE, hy - HANDLE, 2 * HANDLE, 2 * HANDLE)
                    cr.set_source_rgb(*ACCENT)
                    cr.fill_preserve()
                    cr.set_source_rgb(*theme.rgb("bg"))
                    cr.set_line_width(1.5)
                    cr.stroke()
            if st["mode"] in ("rect", "lasso"):               # розмір вибору
                x, y, w, h = bbox(shape)
                cr.select_font_face(theme.FONT["icons"])
                cr.set_font_size(12)
                txt = f"{round(w * S)} × {round(h * S)}"
                cr.move_to(x + 4, y - 6 if y > 20 else y + h + 16)
                cr.set_source_rgb(*ACCENT)
                cr.show_text(txt)
        return False

    def done():
        shape = current_shape()
        if not shape:
            return
        if shape[0] == "rect" and (shape[3] < 3 or shape[4] < 3):
            return
        st["result"] = shape
        win.hide()
        Gtk.main_quit()

    def on_press(_w, ev):
        if ev.button == 3:                                    # правий клік — скасувати
            Gtk.main_quit()
            return True
        if ev.button != 1:
            return True
        x, y = ev.x, ev.y
        if st["mode"] in ("rect", "lasso") and st["sel"]:
            h = hit(x, y)
            if h:                                             # тягнемо маркер / переносимо вибір
                st["adjust"] = (h, (x, y), st["sel"])
                return True
            st["sel"] = None                                  # клік повз — почати новий вибір
            refresh_buttons()
        if st["mode"] == "rect":
            st["drag"] = [(x, y), (x, y)]
        elif st["mode"] == "lasso":
            st["pts"] = [(x, y)]
        elif st["mode"] == "window":
            st["hover"] = window_at(x, y)
            done()
        elif st["mode"] == "screen":
            done()
        return True

    def on_motion(_w, ev):
        x, y = ev.x, ev.y
        if st["adjust"]:
            h, (sx, sy), orig = st["adjust"]
            st["sel"] = reshape(orig, h, x - sx, y - sy)
            area.queue_draw()
            return True
        if st["mode"] in ("rect", "lasso") and st["sel"] and not st["drag"] and not st["pts"]:
            set_cursor(hit(x, y))                             # курсор підказує, що можна тягнути
            return True
        if st["mode"] == "rect" and st["drag"]:
            st["drag"][1] = (x, y)
        elif st["mode"] == "lasso" and st["pts"]:
            lx, ly = st["pts"][-1]
            if math.hypot(x - lx, y - ly) >= 2:              # не тягнути зайвих точок
                st["pts"].append((x, y))
        elif st["mode"] == "window":
            st["hover"] = window_at(x, y)
        else:
            return True
        area.queue_draw()
        return True

    def on_release(_w, ev):
        if ev.button != 1:
            return True
        if st["adjust"]:                                      # відпустили маркер — вибір лишається
            st["adjust"] = None
        elif st["mode"] == "rect" and st["drag"]:             # обвели — тепер можна підправити, знімок — Enter
            (x0, y0), (x1, y1) = st["drag"]
            st["drag"] = None
            if abs(x1 - x0) >= 3 and abs(y1 - y0) >= 3:
                st["sel"] = ("rect", min(x0, x1), min(y0, y1), abs(x1 - x0), abs(y1 - y0))
        elif st["mode"] == "lasso" and st["pts"]:
            if len(st["pts"]) > 2:
                st["sel"] = ("poly", st["pts"])
            st["pts"] = []
        refresh_buttons()
        area.queue_draw()
        return True

    def on_key(_w, ev):
        k = Gdk.keyval_name(ev.keyval) or ""
        # Ctrl+C (і на українській розкладці — Ctrl+С) — зняти вибране й лише скопіювати в буфер
        if ev.state & Gdk.ModifierType.CONTROL_MASK:
            if k.lower() in ("c", "cyrillic_es") and not st["record"] and current_shape():
                st["copy"] = True
                done()
            return True
        modes = {"1": "rect", "r": "rect", "2": "window", "w": "window",
                 "3": "lasso", "l": "lasso", "4": "screen", "f": "screen"}
        if k == "Escape":
            if st["sel"]:                                     # спершу — скинути вибір, потім — вийти
                st["sel"], st["adjust"] = None, None
                refresh_buttons()
                set_cursor(None)
                area.queue_draw()
            else:
                Gtk.main_quit()
        elif k in ("Tab", "ISO_Left_Tab"):                    # Tab — наступний режим, Shift+Tab — попередній
            order = ["rect", "window", "lasso", "screen"]
            step = -1 if (k == "ISO_Left_Tab" or ev.state & Gdk.ModifierType.SHIFT_MASK) else 1
            set_mode(order[(order.index(st["mode"]) + step) % len(order)])
        elif k.lower() in modes:
            set_mode(modes[k.lower()])
        elif k.lower() == "e":
            toggle_edit()
        elif k.lower() == "v":
            set_record(not st["record"])
        elif k.lower() == "a":
            toggle_audio()
        elif k in ("Return", "KP_Enter", "space"):
            if st["mode"] == "screen" or st["sel"]:
                done()
        return True

    area.connect("draw", on_draw)
    area.connect("button-press-event", on_press)
    area.connect("motion-notify-event", on_motion)
    area.connect("button-release-event", on_release)
    win.connect("key-press-event", on_key)
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    refresh_buttons()                                         # після show_all: сховати непотрібну опцію

    # Самоперевірка без миші: SNIP_TEST="lasso" | "rect" | "window" — імітувати вибір і завершити
    test = os.environ.get("SNIP_TEST")
    if test:
        from gi.repository import GLib
        set_mode("rect" if test == "tab" else test)
        if test == "tab":                                     # Tab, Tab, Shift+Tab → «вікно»
            seq = []
            for name in ("Tab", "Tab", "ISO_Left_Tab"):
                ev = Gdk.Event.new(Gdk.EventType.KEY_PRESS)
                ev.keyval = Gdk.keyval_from_name(name)
                on_key(win, ev)
                seq.append(st["mode"])
            print("tab:", " → ".join(seq), flush=True)
            GLib.timeout_add(600, lambda: Gtk.main_quit() or False)
        elif test == "lasso":
            st["pts"] = [(MW / 2 + 220 * math.cos(a) * (1 + 0.25 * math.sin(3 * a)),
                          MH / 2 + 160 * math.sin(a)) for a in [i * math.pi / 40 for i in range(80)]]
        elif test == "rect":
            st["drag"] = [(300, 250), (900, 650)]
        elif test == "adjust":                                # вибір → правий нижній маркер +100 → перенести на (−50, +20)
            st["sel"] = ("rect", 300, 250, 600, 400)
            st["sel"] = reshape(st["sel"], "se", 100, 100)
            st["sel"] = reshape(st["sel"], "move", -50, 20)
            refresh_buttons()
            print("adjust:", tuple(round(v) for v in bbox(st["sel"])), "кнопка «знімок» видна:", sb.get_visible(), flush=True)
        elif test == "copy":                                  # вибір → Ctrl+C → лише буфер обміну
            st["sel"] = ("rect", 300, 250, 400, 300)
            def ctrl_c():
                ev = Gdk.Event.new(Gdk.EventType.KEY_PRESS)
                ev.keyval = Gdk.keyval_from_name("c")
                ev.state = Gdk.ModifierType.CONTROL_MASK
                on_key(win, ev)
                return False
            GLib.timeout_add(500, ctrl_c)
        elif test == "window":
            st["hover"] = wins[0] if wins else None
        area.queue_draw()
        GLib.timeout_add(1800, lambda: done() or False)
    Gtk.main()

    shape = st["result"]
    if not shape:
        return
    if st["record"]:                                          # ЗАПИС: область = прямокутник навколо вибраного
        os.unlink(frozen)
        if shape[0] == "rect" and st["mode"] == "screen":
            return rec_start(output=mon["name"], audio=st["audio"])
        if shape[0] == "rect":
            _, x, y, w, h = shape
        else:
            xs, ys = [p[0] for p in shape[1]], [p[1] for p in shape[1]]
            x, y, w, h = min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)
        w, h = int(w) // 2 * 2, int(h) // 2 * 2                # h264 — лише парні розміри
        return rec_start(geom=f"{int(x) + mon['x']},{int(y) + mon['y']} {w}x{h}", audio=st["audio"])
    # ── вирізати вибране з замороженого знімка (у пікселях знімка)
    if shape[0] == "rect":
        _, x, y, w, h = shape
        bx, by, bw, bh = x, y, w, h
    else:
        xs, ys = [p[0] for p in shape[1]], [p[1] for p in shape[1]]
        bx, by = min(xs), min(ys)
        bw, bh = max(xs) - bx, max(ys) - by
    px, py = max(0, int(bx * S)), max(0, int(by * S))
    pw = min(img.get_width() - px, math.ceil(bw * S))
    ph = min(img.get_height() - py, math.ceil(bh * S))
    out = cairo.ImageSurface(cairo.FORMAT_ARGB32, pw, ph)     # прозорий фон (для ласо)
    cr = cairo.Context(out)
    if shape[0] == "poly":                                    # ласо: лише всередині обведеного
        cr.move_to(shape[1][0][0] * S - px, shape[1][0][1] * S - py)
        for (x, y) in shape[1][1:]:
            cr.line_to(x * S - px, y * S - py)
        cr.close_path()
        cr.clip()
    cr.set_source_surface(img, -px, -py)
    cr.paint()
    os.unlink(frozen)
    if st["copy"]:                                            # Ctrl+C: лише буфер обміну, без файлу
        import io
        buf = io.BytesIO()
        out.write_to_png(buf)
        subprocess.run(["wl-copy", "--type", "image/png"], input=buf.getvalue())
        notify("Знімок скопійовано в буфер обміну")
        return
    path = out_path()
    out.write_to_png(path)
    finish(path, st["edit"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="rect", choices=["rect", "window", "lasso", "screen"])
    ap.add_argument("--no-edit", action="store_true")
    ap.add_argument("--now", action="store_true")
    ap.add_argument("--video", action="store_true")
    a = ap.parse_args()
    lock = open(os.path.join(RUN, "snip.lock"), "w")         # лише один вибір за раз
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return
    if rec_stop():                                            # запис іде — будь-який виклик його зупиняє
        return
    if a.video:
        select(a.mode, False, record=True)
    elif a.now:
        instant(a.mode, not a.no_edit)
    else:
        select(a.mode, not a.no_edit)


if __name__ == "__main__":
    main()
