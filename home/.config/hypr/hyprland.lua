-- ╭──────────────────────────────────────────────────────────────╮
-- │  HYPRLAND — головний файл                                    │
-- │  Тема «Тиха ніч»: суцільні кольори, без блюру й тіней,       │
-- │  короткі анімації → мінімальне навантаження на відеочіп.     │
-- │  Вікі: https://wiki.hypr.land/configuring/                   │
-- ╰──────────────────────────────────────────────────────────────╯
--
-- Структура:
--   hyprland.lua   — монітор, автозапуск, змінні середовища, ввід, misc
--   colors.lua     — тема: кольори й розміри з ~/.config/theme/theme.conf
--   appearance.lua — вигляд: рамки, відступи, заокруглення, анімації
--   keyBinds.lua   — усі комбінації клавіш
--   rules.lua      — правила для вікон і шарів (waybar, rofi)


-- ───────────── МОНІТОР ─────────────
-- https://wiki.hypr.land/configuring/core/monitors/
hl.monitor({
    output   = "eDP-1",       -- назва виходу (дивись `hyprctl monitors`)
    mode     = "1920x1080",   -- роздільна здатність (можна "1920x1080@60")
    position = "0x0",         -- позиція монітора (0x0: інакше X11-програми, напр. Resolve, промахуються курсором)
    scale    = "1",           -- масштаб інтерфейсу
})
-- Будь-який інший монітор (інший комп'ютер, зовнішній екран) — рідна роздільна здатність, праворуч
hl.monitor({
    output   = "",            -- порожньо = усі виходи, для яких немає окремого правила
    mode     = "preferred",
    position = "auto",
    scale    = "1",
})


-- ───────────── ПРОГРАМИ ─────────────
-- Глобальні змінні: їх використовує keyBinds.lua
-- Звичайні термінали — ОДИН процес kitty на всі вікна (~45 МБ пам'яті економії на кожне):
-- SUPER+Enter, scratchpad (SUPER+S), Claude. Панелі «Музика» й «Зв'язок» — окремо (свої налаштування).
terminal    = "kitty --single-instance --instance-group=term"
fileManager = "dolphin"
browser     = "firefox"
launcher    = "~/.config/rofi/scripts/launcher.sh"    -- універсальне меню (програми, ігри, файли, вікна, математика, посилання)
powermenu   = "~/.config/rofi/scripts/powermenu.sh"   -- меню живлення
locker      = "hyprlock"                              -- блокування екрана


-- ───────────── АВТОЗАПУСК ─────────────
-- https://wiki.hypr.land/configuring/core/autostart/
hl.on("hyprland.start", function ()
    hl.exec_cmd("GTK_MODULES=$HOME/.local/lib/waybar-islands.so waybar")       -- панель (модуль islands: кліки лише по острівцях, див. src/islands.c)
    hl.exec_cmd("hyprpaper")    -- шпалери (статичні — 0 навантаження після старту)
    hl.exec_cmd("command -v swaync >/dev/null && swaync || dunst")  -- сповіщення: swaync (стопки як в One UI), якщо не встановлено — dunst
    hl.exec_cmd(os.getenv("HOME") .. "/.nix-profile/bin/wlr-screen-corners --radius 18 --curvature 0") -- заокруглені кути екрана
    -- економ після входу: thermald (--adaptive) при старті ставить справжній ліміт (MMIO) 24 Вт, і сервіс power-limit-10w його не перебиває
    hl.exec_cmd("sleep 5; sudo -n /usr/local/bin/power-mode eco >/dev/null 2>&1; pkill -RTMIN+17 -x waybar")
    -- термінали в scratchpad запускає startup.lua
    hl.exec_cmd(os.getenv("HOME") .. "/.config/hypr/services.sh")   -- фонові служби: пароль (polkit), автоблокування, нічне світло, флешки, буфер обміну…
end)
-- Після перечитування конфігу (SUPER+Shift+R) — дозапустити служби, яких бракує (напр. щойно встановлені)
hl.on("config.reloaded", function () hl.exec_cmd(os.getenv("HOME") .. "/.config/hypr/services.sh") end)


-- ───────────── ЗМІННІ СЕРЕДОВИЩА ─────────────
-- https://wiki.hypr.land/configuring/core/environment-variables/
hl.env("XCURSOR_SIZE",    "24")           -- розмір курсора (X11-програми)
hl.env("HYPRCURSOR_SIZE", "24")           -- розмір курсора (Wayland)
hl.env("QT_QPA_PLATFORM", "wayland;xcb")  -- Qt-програми (dolphin) нативно у Wayland
hl.env("MOZ_ENABLE_WAYLAND", "1")         -- Firefox нативно у Wayland


-- ───────────── ДОЗВОЛИ ─────────────
-- https://wiki.hypr.land/configuring/core/advanced-configuration/permissions/
-- hl.config({ ecosystem = { enforce_permissions = true } })   -- увімкнути систему дозволів
-- hl.permission("/usr/(bin|local/bin)/grim", "screencopy", "allow")


-- ───────────── ПІДКЛЮЧЕННЯ МОДУЛІВ ─────────────
require("colors")        -- тема: кольори C і розміри THEME (~/.config/theme/theme.conf)
require("appearance")    -- вигляд + анімації
require("waybarAuto")    -- автопоказ waybar (rofi / порожній стіл)
require("comms")         -- права панель «Зв'язок» (SUPER+M: nchat тощо)
require("music")         -- ліва панель «Музика» (SUPER+A: kew + пошук YouTube Music)
require("keyBinds")      -- клавіші
require("rules")         -- правила вікон/шарів
require("fullscreen")    -- повний екран, коли поруч є вікна, — лише у своїй частині екрана
require("tablet")        -- графічний планшет: налаштування з віджета «Планшет» (TabletApply)
require("startup")       -- стартові вікна: Claude Code + термінал групою в scratchpad (SUPER+S)


-- ───────────── MISC ─────────────
hl.config({
    misc = {
        force_default_wallpaper  = 0,      -- 0 = не малювати стандартні шпалери Hyprland
        disable_hyprland_logo    = true,   -- вимкнути логотип/аніме-дівчинку на фоні
        disable_splash_rendering = true,   -- вимкнути текст-заставку внизу екрана
        background_color         = C.bg,   -- колір фону, якщо шпалер немає
        vrr                      = 0,      -- змінна частота оновлення: 0 = вимк (вбудований екран її не підтримує)
        render_unfocused_fps     = 10,     -- FPS для прихованих вікон (економія GPU, типово 15)
        animate_manual_resizes   = false,  -- не анімувати ручну зміну розміру (менше кадрів)
        animate_mouse_windowdragging = false, -- не анімувати перетягування мишкою
        mouse_move_enables_dpms  = true,   -- рух миші вмикає вимкнений екран
        key_press_enables_dpms   = true,   -- натискання клавіші вмикає вимкнений екран
        focus_on_activate        = false,  -- не перемикати фокус, коли програма просить уваги
        enable_swallow           = false,  -- «ковтання» вікна терміналом програмою з нього
        disable_autoreload       = false,  -- автоматично перечитувати конфіг після збереження
        enable_anr_dialog        = true,   -- діалог «програма не відповідає»
    },

    -- Рендер
    render = {
        direct_scanout = 1,                -- повноекранні програми/ігри рендеряться напряму, оминаючи композитор (1 = увімк)
    },

    -- Курсор
    cursor = {
        inactive_timeout  = 5,             -- ховати курсор після 5 с бездіяльності (0 = ніколи)
        hide_on_key_press = false,         -- ховати курсор під час набору тексту
        no_hardware_cursors = 0,           -- 0 = апаратний курсор (не перемальовує кадр при русі миші)
    },

    -- Шари (waybar, rofi тощо)
    layers = {
        allow_new_top_layers_over_existing_fullscreen = true, -- нові шари (rofi) поверх повноекранного вікна
    },

    -- Екосистема
    ecosystem = {
        no_update_news   = true,           -- не показувати вікно новин після оновлення
        no_donation_nag  = true,           -- не показувати нагадування про донат
    },
})


-- ───────────── ВВІД ─────────────
hl.config({
    input = {
        kb_layout  = "us, ua",           -- розкладки
        kb_variant = "",                 -- варіанти розкладок
        kb_model   = "",                 -- модель клавіатури
        kb_options = "grp:caps_toggle",  -- перемикання розкладки: CapsLock
        kb_rules   = "",

        numlock_by_default = true,       -- NumLock увімкнений при старті
        repeat_rate  = 35,               -- швидкість повтору клавіші (символів/с)
        repeat_delay = 280,              -- затримка до початку повтору (мс)

        follow_mouse = 1,                -- 1 = фокус іде за курсором
        sensitivity  = 0,                -- чутливість миші: -1.0 … 1.0 (0 = без змін)
        accel_profile = "flat",          -- "flat" = без прискорення миші, "adaptive" = з прискоренням

        touchpad = {
            natural_scroll       = true,  -- «природна» прокрутка, як на Mac: контент їде за пальцем (миші це не стосується)
            tap_to_click         = true,  -- тап = клік
            disable_while_typing = true,  -- вимикати тачпад під час набору
            scroll_factor        = 0.8,   -- швидкість прокрутки
            clickfinger_behavior = true,  -- натискання 1/2/3 пальцями = лівий/правий/середній клік у будь-якому місці (як на Mac)
            tap_button_map       = "lrm", -- тап 1/2/3 пальцями = лівий/правий/середній клік
            tap_and_drag         = true,  -- подвійний тап і тягнути = перетягування
            drag_lock            = 0,     -- 0 = відпустив палець — перетягування закінчилось
            drag_3fg             = 1,     -- перетягування ТРЬОМА пальцями без натискання (як на Mac); 0 = вимкнути
        },
    },
})

-- ───────────── ПРИСКОРЕННЯ ТАЧПАДА (як на MacBook) ─────────────
-- Курсор і прокрутка двома пальцями мають СВОЇ криві: швидкість залежить лише від того,
-- наскільки швидко рухається палець (прискорив під кінець — курсор / сторінка теж пришвидшились).
-- Повільно — точно (менше за палець), швидко — далеко. Лише для тачпада: миша — "flat" вище.
-- Крива: множник = MIN + (MAX − MIN) · v² / (v² + KNEE²), v — швидкість пальця
-- (1 ≈ 25 мм/с — повільний рух; 10+ — швидкий змах).
-- Змінив число → SUPER+Shift+R (перечитати) і пробуй.
local TP_POINTER = { min = 0.5, max = 3.4, knee = 4.0 }   -- курсор: точний на повільному, ×3.4 на змаху
local TP_SCROLL  = { min = 1.2, max = 8.0, knee = 5.0 }   -- прокрутка (×2 від початкових 0.6 / 4.0): повільно — по рядку, змах — на кілька екранів
local TP_STEP    = 1.0                                    -- крок точок кривої (швидкість v)

-- Точки кривої для libinput: «крок т0 т1 …», де т — вихідна швидкість при v = 0, 1, 2 … 20
-- (далі libinput продовжує лінійно)
local function tp_points(c)
    local pts = {}
    for i = 0, 20 do
        local v = i * TP_STEP
        local gain = c.min + (c.max - c.min) * v * v / (v * v + c.knee * c.knee)
        pts[#pts + 1] = string.format("%.3f", v * gain)
    end
    return TP_STEP .. " " .. table.concat(pts, " ")
end

hl.device({
    name          = "syna2ba6:00-06cb:ce2d-touchpad",   -- тачпад ноутбука (hyprctl devices)
    accel_profile = "custom " .. tp_points(TP_POINTER),  -- курсор
    scroll_points = tp_points(TP_SCROLL),                -- прокрутка двома пальцями
})

-- ───────────── ЖЕСТИ ТАЧПАДА (як на Mac) ─────────────
-- 3 пальці зайняті перетягуванням (drag_3fg вище), тому жести — на 4 пальцях, як на Mac.
-- 4 пальці ← →  — перемикання робочих столів (анімація йде за пальцями)
hl.gesture({
    fingers   = 4,
    direction = "horizontal",
    action    = "workspace",
})

-- 4 пальці ↑ / ↓ — протилежний жест закриває те, що відкрив інший (як на Mac):
--   ↑  відкритий scratchpad → сховати його; інакше — меню rofi + waybar (як SUPER+R; ще раз — закрити)
--   ↓  відкритий rofi → закрити його (і waybar); інакше — показати / сховати scratchpad (як SUPER+S)
local function scratch_open()
    local w = hl.get_active_special_workspace()
    return w ~= nil and w.name == "special:magic"
end

hl.gesture({
    fingers   = 4,
    direction = "up",
    action    = function ()
        if scratch_open() then
            hl.dispatch(hl.dsp.workspace.toggle_special("magic"))   -- сховати scratchpad
        else
            SuperTest("r")                                          -- rofi: відкрити / закрити
        end
    end,
})

hl.gesture({
    fingers   = 4,
    direction = "down",
    action    = function ()
        if RofiIsOpen() then
            SuperTest("r")                                          -- rofi відкритий → закрити все
        else
            hl.dispatch(hl.dsp.workspace.toggle_special("magic"))   -- scratchpad: показати / сховати
        end
    end,
})

-- Щипок 2 пальцями (без SUPER) — плавний зум у будь-якій програмі робить демон ~/.local/bin/pinch-zoom
-- (Ctrl + дрібна прокрутка під час щипка; запускається з services.sh). Тут щипок НЕ прив'язуємо.

-- SUPER + зведення/розведення 2 пальців — лупа всього екрана (зум іде за пальцями, центр — курсор).
hl.gesture({
    fingers    = 2,
    direction  = "pinch",
    mods       = "SUPER",
    action     = "cursorZoom",
    zoom_level = 1,          -- крок/множник масштабу (1 = без додаткового підсилення)
    mode       = "live",     -- "live" = масштаб плавно йде за пальцями
})

hl.config({
    gestures = {
        workspace_swipe_distance          = 250,  -- скільки пікселів «проїхати» пальцями на повний перехід (менше = легше; 700 було надто важко)
        workspace_swipe_cancel_ratio      = 0.15, -- провів хоча б 15 % шляху й відпустив — сторінка перегорнеться (менше = легше)
        workspace_swipe_min_speed_to_force = 5,   -- легкий швидкий змах перемикає навіть на короткій дистанції (менше = легше)
        workspace_swipe_direction_lock    = true, -- почав вліво/вправо — вертикальний рух не плутає жест
    },
})


-- ───────────── ЛЕЙАУТИ ─────────────
hl.config({
    dwindle = {
        preserve_split = true,   -- зберігати напрямок поділу вікон
        smart_split    = false,  -- поділ залежно від позиції курсора
        smart_resizing = true,   -- розумна зміна розміру мишкою
    },
    master = {
        new_status = "master",   -- нове вікно стає головним
    },
})
