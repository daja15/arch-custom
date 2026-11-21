-- ╭──────────────────────────────────────────────────────────────╮
-- │  ПРАВИЛА ВІКОН ТА ШАРІВ                                      │
-- │  https://wiki.hypr.land/configuring/core/rules/              │
-- │  Кожне правило можна вимкнути: rule:set_enabled(false)       │
-- ╰──────────────────────────────────────────────────────────────╯


-- ───────────── ВІКНА ─────────────

-- Ігнорувати запити програм «розгорни мене» (вікна лишаються в сітці)
hl.window_rule({
    name  = "suppress-maximize-events",
    match = { class = ".*" },
    suppress_event = "maximize",
})

-- Виправлення перетягування у XWayland-програмах
hl.window_rule({
    name  = "fix-xwayland-drags",
    match = {
        class = "^$", title = "^$",
        xwayland = true, float = true, fullscreen = false, pin = false,
    },
    no_focus = true,
})

-- Діалоги й дрібні утиліти — плаваючі
hl.window_rule({
    name  = "float-dialogs",
    match = { title = "^(Open File|Save File|Відкрити файл|Зберегти файл|Вибір файлу|Properties|Властивості)(.*)$" },
    float = true,
})

-- Picture-in-Picture у Firefox — плаваюче й закріплене на всіх столах
hl.window_rule({
    name  = "firefox-pip",
    match = { title = "^(Picture-in-Picture|Картинка в картинці)$" },
    float = true,
    pin   = true,
})

-- Перегляд медіа з nchat (mpv / imv) — плаваюче вікно поверх чату, а не в сітці
hl.window_rule({
    name  = "nchat-media",
    match = { class = "^(mpv|imv)$" },
    float = true,
})

-- Hyprland-run (вбудований запускач) — плаваючий, внизу ліворуч
hl.window_rule({
    name  = "move-hyprland-run",
    match = { class = "hyprland-run" },
    move  = "20 monitor_h-120",
    float = true,
})


-- ───────────── ШАРИ (layer-shell: waybar, rofi, сповіщення) ─────────────

-- Rofi: без блюру.
-- Rofi займає весь екран прозорою поверхнею: клік по ній (будь-де, крім меню
-- й waybar) закриває rofi — це робить click-to-exit у rofi/config.rasi.
-- Те, що waybar лежить НАД rofi, забезпечує order = -1 у правилі waybar нижче.
hl.layer_rule({
    name      = "rofi-anim",
    match     = { namespace = "^rofi$" },
    blur      = false,          -- без блюру
    no_anim   = true,           -- з'являється миттєво: анімація повноекранної поверхні rofi коштувала
                                -- ~5% ядра Hyprland і ~6% відеокарти на кожне відкриття (замір 2026-11-15).
                                -- Повернути «випливання»: прибери цей рядок і додай  animation = "popin 92%",
})

-- Waybar: без анімації Hyprland. Схована панель не зникає, а стає прозорою
-- (waybar лише перемикає шар), тож анімації шарів на ній не спрацьовують.
-- Панель з'являється й зникає миттєво, без жодної анімації (див. waybarAuto.lua).
hl.layer_rule({
    name    = "waybar-no-anim",
    match   = { namespace = "^waybar$" },
    no_anim = true,
    order   = -1,               -- завжди НАД rofi на тому ж шарі overlay (менше число = вище),
                                -- хоч би хто з'явився пізніше: панель не треба перепіднімати
})

-- Шпалери (hyprpaper) — завжди під віджетами столу (коло, календар, погода…),
-- хоч би хто запустився першим: усі вони на одному шарі «background».
-- order: більше число = ближче до «дна» екрана (нижче за інших).
hl.layer_rule({
    name    = "wallpaper-bottom",
    match   = { namespace = "^hyprpaper$" },
    order   = 10,
    no_anim = true,
})

-- Зірки (~/.local/bin/stars, desk.sh toggle stars) — прозорий шар на весь екран.
-- Якщо в stars.c вибрано «під вікнами» — над шпалерами, але під віджетами столу
-- (у віджетів order 0, у шпалер 10). Без анімації появи шару.
hl.layer_rule({
    name    = "stars-layer",
    match   = { namespace = "^stars$" },
    order   = 5,
    no_anim = true,
})

-- Висувні панелі столу (desk: Wi-Fi, Bluetooth, Ігри, Dev-стек): виїжджають самі (GTK, ease-in / ease-out),
-- анімація шарів Hyprland на зміні розміру лише заважала б («тягнула» вікно)
hl.layer_rule({
    name    = "desk-drawer-no-anim",
    match   = { namespace = "^desktop-drawer-(top|bottom)$" },
    no_anim = true,
})

-- Екран-селектор / оверлеї скриншотів — без анімацій
-- (snip — вибір області для знімка, snip.py: має з'явитись миттєво, як «заморожений» екран)
hl.layer_rule({
    name    = "selection-no-anim",
    match   = { namespace = "^(selection|hyprpicker|snip)$" },
    no_anim = true,
})


-- ───────────── РЕДАКТОР ЗНІМКІВ (satty) ─────────────
-- Плаваюче вікно по центру, 80% екрана (див. hypr/snip.py)
hl.window_rule({
    name   = "satty",
    match  = { class = "^(com.gabm.satty|satty)$" },
    float  = true,
    size   = "monitor_w*0.8 monitor_h*0.8",
    center = true,
})


-- ───────────── ДИСПЕТЧЕР ЗАВДАНЬ (btop з меню Ctrl+Alt+Del) ─────────────
-- Плаваюче вікно по центру, 70% екрана
hl.window_rule({
    name   = "taskmgr",
    match  = { class = "^taskmgr$" },
    float  = true,
    size   = "monitor_w*0.7 monitor_h*0.7",
    center = true,
})


-- ───────────── ПЕРЕГЛЯДАЧ ФОТО (swayimg) ─────────────
-- Плаваюче вікно по центру, 80% екрана: відкрив фото — подивився — закрив (q / Esc)
hl.window_rule({
    name   = "swayimg",
    match  = { class = "^swayimg$" },
    float  = true,
    size   = "monitor_w*0.8 monitor_h*0.8",
    center = true,
})


-- ───────────── ОНОВЛЕННЯ СИСТЕМИ (клік по модулю оновлень у waybar) ─────────────
hl.window_rule({
    name   = "floating-update",
    match  = { class = "^floating-update$" },
    float  = true,
    size   = "1000 640",
    center = true,
})

-- ───────────── СХОВОК (SUPER+S, special:magic) ─────────────
-- Відступи від країв екрана — на 8 px більші, ніж на звичайних столах (theme.conf: gaps-top, gaps-out)
local SCRATCH_EXTRA = 8
hl.workspace_rule({
    workspace = "special:magic",
    gaps_out  = { top    = THEME.gaps_top + SCRATCH_EXTRA, right = THEME.gaps_out + SCRATCH_EXTRA,
                  bottom = THEME.gaps_out + SCRATCH_EXTRA, left  = THEME.gaps_out + SCRATCH_EXTRA },
})

-- Стартові термінали (startup.lua) — тихо в scratchpad; Claude створює групу, термінал до неї приєднується
hl.window_rule({
    name      = "scratch-claude",
    match     = { class = "^scratch-claude$" },
    workspace = "special:magic silent",   -- у сховок, не перемикаючись туди
    group     = "set",                    -- одразу як група-вкладки
})
hl.window_rule({
    name      = "scratch-term",
    match     = { class = "^scratch-term$" },
    workspace = "special:magic silent",   -- у сховок, не перемикаючись туди
})
