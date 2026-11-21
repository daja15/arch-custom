-- ╭──────────────────────────────────────────────────────────────╮
-- │  ЛІВА ПАНЕЛЬ «МУЗИКА» — kew + пошук YouTube Music            │
-- │  SUPER+A — висунути / сховати панель                          │
-- │  Курсор до середини лівого краю — теж висуває (desk)          │
-- │                                                              │
-- │   край екрана → ┌───────────────┐                            │
-- │                 │ Пошук│Бібліотека  ← вкладки kitty          │
-- │                 │  music-find / kew│                         │
-- │                 └───────────────┘                            │
-- │                                                              │
-- │  Одне плаваюче вікно kitty (клас music-panel). Сховане лежить│
-- │  у схованці special:music (ніколи не показується), висунуте —│
-- │  закріплене на твоєму столі, виїжджає з лівого краю          │
-- │  (windowsMove). Див. panel.lua. Нове вікно ховає панель.     │
-- │  Музика грає й тоді, коли панель схована.                    │
-- │  Клік по будь-якому іншому вікну ховає панель.               │
-- │  Праворуч від терміналу — смуга іконок waybar-music          │
-- │  (waybar/music.jsonc): пошук, ⏮ ⏯ ⏭, повтор, скачано.        │
-- │  Працює на подіях Hyprland — у спокої 0 навантаження.        │
-- ╰──────────────────────────────────────────────────────────────╯

local P    = require("panel")
local HOME = os.getenv("HOME")

-- ───────────── НАЛАШТУВАННЯ ─────────────
local PANEL_W = 640                                   -- ширина панелі (px); на всю висоту екрана
local ANIM_MS = ECO and 300 or 500                    -- скільки триває ковзання (≈ windowsMove у appearance.lua)
local CLASS   = "music-panel"                         -- клас вікна kitty (задає music-launch.sh)
local WS      = "music"                               -- схованка special:music (ніколи не показується)
local LAUNCH  = HOME .. "/.config/hypr/music-launch.sh"
local MENU_BIN = HOME .. "/.local/bin/waybar-music"  -- той самий waybar під іншим іменем: сигнали основної панелі його не чіпають

-- ───────────── ПРАВИЛА ─────────────
-- Вікно одразу народжується плаваючим у схованці (за край його ставить window.open)
hl.window_rule({
    name      = "music-panel",
    match     = { class = "^" .. CLASS .. "$" },
    float     = true,
    workspace = "special:" .. WS .. " silent",
    size      = PANEL_W .. " monitor_h",
    move      = "0 0",
})

-- ───────────── СТАН ─────────────
local shown    = false   -- панель висунута (або висувається)
local menu_shown = nil   -- смуга іконок показана
local hide_gen = 0       -- номер «приховання»: новий показ скасовує незавершене приховання

local function is_open() return shown end

local function panel_window()
    for _, w in ipairs(hl.get_windows() or {}) do
        if w.class == CLASS then return w end
    end
end

-- Поставити вікно: x — від лівого краю монітора. Зміна позиції анімується.
local function place(w, x)
    local m = hl.get_active_monitor()
    if not (w and m) then return end
    local sel = "address:" .. w.address
    hl.dispatch(hl.dsp.window.resize({ x = PANEL_W, y = m.height, window = sel }))
    hl.dispatch(hl.dsp.window.move({ x = m.x + x, y = m.y, window = sel }))
end

-- Смуга іконок (waybar-music): USR1 = показати, USR2 = сховати
local function menu(on)
    if on == menu_shown then return end
    menu_shown = on
    hl.exec_cmd("pkill " .. (on and "-USR1" or "-USR2") .. " -x waybar-music")
end

-- «Ловець кліків» у програмі desk: поки панель висунута, клік повз неї (і повз смугу іконок) ховає її.
-- Файл music-holes — де кліки НЕ ловити; SIGWINCH — «стан змінився».
local HOLES = (os.getenv("XDG_RUNTIME_DIR") or "/tmp") .. "/music-holes"
local STRIP_W = 64                               -- смуга іконок праворуч від панелі
local function catcher(on)
    if on then
        local m = hl.get_active_monitor()
        local f = io.open(HOLES, "w")
        if f and m then f:write(string.format("0 0 %d %d\n", PANEL_W + STRIP_W, m.height)); f:close() end
    else
        os.remove(HOLES)
    end
    hl.exec_cmd("pkill -WINCH -x desk")
end

local OFF_X = -PANEL_W - 20                     -- сховане: за лівим краєм

-- ───────────── ПОКАЗ / ПРИХОВАННЯ ─────────────
local function slide_in(w, retry)
    P.bring(w)                                  -- зі схованки на поточний стіл (там воно за краєм)
    place(w, 0)
    hl.dispatch(hl.dsp.focus({ window = P.sel(w) }))
    menu(true)                                  -- іконки — одразу, разом з панеллю
    if retry then return end
    -- Страховка: зрідка вікно переноситься на стіл, але не рушає з-за краю — тоді ще раз
    hl.timer(function ()
        local x = panel_window()
        local m = hl.get_active_monitor()
        if shown and x and m and x.at and x.at.x < m.x then slide_in(x, true) end
    end, { timeout = 150, type = "oneshot" })
end

local function show()
    shown = true
    hide_gen = hide_gen + 1                     -- скасувати незавершене приховання
    P.close_special()                           -- сховок SUPER+S закрити — інакше панель під ним
    if WaybarPanelLeaving then WaybarPanelLeaving(WS, false) end  -- панель на екрані
    local w = panel_window()
    if w then
        slide_in(w)
    else
        hl.exec_cmd(LAUNCH)                     -- ще не запущена — window.open висуне її
        local gen = hide_gen
        hl.timer(function ()                    -- не запустилась (немає пакетів — див. сповіщення) — скинути стан
            if gen == hide_gen and not panel_window() then
                shown = false
                catcher(false)
                if WaybarPanelLeaving then WaybarPanelLeaving(WS, nil) end
            end
        end, { timeout = 4000, type = "oneshot" })
    end
    catcher(true)
end

local function hide()
    shown = false
    menu(false)
    catcher(false)
    local w = panel_window()
    local addrs = {}
    if w then
        addrs[w.address] = true
        if P.out(w) then place(w, OFF_X) end
    end
    hide_gen = hide_gen + 1
    local gen = hide_gen
    if WaybarPanelLeaving then WaybarPanelLeaving(WS, true, addrs) end   -- waybar — одразу, не чекаючи кінця ковзання
    hl.timer(function ()                        -- коли вікно доїхало — у схованку
        if gen ~= hide_gen then return end
        local x = panel_window()
        if x and P.out(x) then P.stash(x, WS) end
        if WaybarPanelLeaving then WaybarPanelLeaving(WS, nil) end
    end, { timeout = ANIM_MS + 50, type = "oneshot" })
end

-- ───────────── ПОДІЇ ─────────────
-- Фокус пішов на інше вікно (клік повз панель) — сховати
-- Щойно відкриті вікна (адреса → true на 400 мс): фокус на них перевіряємо пізніше —
-- службове вікно на мить (Vimium C у Firefox) не повинно ховати панель
local fresh_open = {}
hl.on("window.open", function (w)
    local addr = w and w.address          -- адресу запам'ятати одразу: коли вікно закриється, w.address стане nil
    if not addr then return end
    fresh_open[addr] = true
    hl.timer(function () fresh_open[addr] = nil end, { timeout = 400, type = "oneshot" })
end)

hl.on("window.active", function ()
    if not shown then return end
    local function check()
        local a = hl.get_active_window()
        if not (shown and a and a.class ~= CLASS) then return end
        if not fresh_open[a.address] then hide(); return end
        local addr = a.address                                   -- нове вікно — глянути ще раз, чи воно живе
        hl.timer(function ()
            if not shown then return end
            if P.fresh(addr) then hide(); return end             -- живе — це справжнє вікно
            local p = panel_window()                             -- зникло — повернути фокус панелі
            if p then hl.dispatch(hl.dsp.focus({ window = P.sel(p) })) end
        end, { timeout = 300, type = "oneshot" })
    end
    hl.timer(check, { timeout = 40, type = "oneshot" })
end)

-- Перший запуск: термінал з'явився в схованці — поставити за край і, якщо
-- панель чекає на нього, висунути. Будь-яке інше нове вікно ховає панель.
hl.on("window.open", function (w)
    if not w then return end
    if w.class ~= CLASS then
        -- Ховати лише якщо вікно прожило 300 мс: службові вікна на мить (Vimium C у Firefox) не рахуються
        if shown then
            local addr = w.address
            hl.timer(function ()
                local x = P.fresh(addr)
                if shown and x and P.out(x) then hide() end
            end, { timeout = 300, type = "oneshot" })
        end
        return
    end
    local m = hl.get_active_monitor()
    if m then hl.dispatch(hl.dsp.window.move({ x = m.x + OFF_X, y = m.y, window = P.sel(w) })) end
    local addr = w.address
    hl.timer(function ()
        local x = P.fresh(addr)
        if x and shown then slide_in(x) end
    end, { timeout = 60, type = "oneshot" })
end)

-- Відкрили сховок SUPER+S — панель ховається (інакше лишилась би під ним)
hl.on("workspace.special_active", function ()
    hl.timer(function ()
        if shown and hl.get_active_special_workspace() then hide() end
    end, { timeout = 40, type = "oneshot" })
end)

-- Вийшли з kitty панелі — скинути стан і прибрати іконки
hl.on("window.close", function ()
    hl.timer(function ()
        if not panel_window() then
            if shown then
                shown = false
                catcher(false)
                if WaybarPanelLeaving then WaybarPanelLeaving(WS, nil) end
            end
            menu(false)
        end
    end, { timeout = 40, type = "oneshot" })
end)

-- Після перечитування конфігу стан скинуто — висунуте вікно повернути в схованку
hl.on("config.reloaded", function ()
    hl.timer(function ()
        local w = panel_window()
        if w and P.out(w) then place(w, OFF_X); P.stash(w, WS) end
    end, { timeout = 200, type = "oneshot" })
end)


-- Запуск смуги іконок (стартує схованою). pgrep — щоб не запустити двічі.
-- Лише після старту Hyprland: під час читання конфігу при вході екран ще не готовий.
local function start_helpers()
    hl.exec_cmd("pgrep -x waybar-music >/dev/null || GTK_MODULES=$HOME/.local/lib/waybar-islands.so " .. MENU_BIN
        .. " -c " .. HOME .. "/.config/waybar/music.jsonc -s " .. HOME .. "/.config/waybar/music.css")
    menu_shown = nil
    hl.timer(function () menu(shown and is_open()) end, { timeout = 1500, type = "oneshot" })
end
hl.on("hyprland.start", start_helpers)
hl.on("config.reloaded", start_helpers)


-- ───────────── ГЛОБАЛЬНІ ФУНКЦІЇ (клавіші, гарячий край) ─────────────

-- SUPER+A: висунути / сховати
function MusicToggle()
    if is_open() and shown then hide() else show() end
end

-- Гарячий лівий край (у програмі desk)
function MusicShow()
    if not (is_open() and shown) then show() end
end

-- Вихід (іконка 󰗼, SUPER+Shift+Q): зупинити музику й закрити панель.
-- Термінал закривається — kew, пошук і mpv завершуються разом з ним.
-- Фонові скачування (окремі процеси) докачуються.
function MusicQuit()
    local w = panel_window()
    shown = false
    menu(false)
    hide_gen = hide_gen + 1
    catcher(false)
    if WaybarPanelLeaving then WaybarPanelLeaving(WS, nil) end
    if w then hl.dispatch(hl.dsp.window.close({ window = "address:" .. w.address })) end
end

function MusicHide()
    if is_open() and shown then hide() end
end
