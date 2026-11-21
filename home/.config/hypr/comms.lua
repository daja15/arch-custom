-- ╭──────────────────────────────────────────────────────────────╮
-- │  ПРАВА ПАНЕЛЬ «ЗВ'ЯЗОК» — Telegram, пошта, Discord           │
-- │  SUPER+M — висунути / сховати панель                          │
-- │                                                              │
-- │   ┌─────────────────────┬──┬───────────┐ ← край екрана       │
-- │   │  VLC (медіа з чату) │ме│  nchat    │                     │
-- │   │                     │ню│ (виїжджає │                     │
-- │   └─────────────────────┴──┴ з краю)  ─┘                     │
-- │                                                              │
-- │  Вікна плаваючі. Сховані лежать у схованці special:comms     │
-- │  (вона ніколи не показується); висунуті — закріплені на      │
-- │  твоєму звичайному столі й виїжджають з правого краю         │
-- │  (анімація windowsMove). Див. panel.lua.                     │
-- │  Нове вікно чи посилання з чату ховає панель.                │
-- │  Меню — окремий waybar «waybar-comms» (waybar/comms.jsonc).  │
-- │  Працює на подіях Hyprland — у спокої 0 навантаження.        │
-- ╰──────────────────────────────────────────────────────────────╯

local P    = require("panel")
local HOME = os.getenv("HOME")
local RUN  = os.getenv("XDG_RUNTIME_DIR") or "/tmp"

-- ───────────── РОЗМІРИ (px) ─────────────
local GAP     = 8      -- відступ між панеллю, меню й VLC (= gaps_out)
local MENU_W  = 210    -- ширина меню з назвами акаунтів (= "width" у waybar/comms.jsonc)
local PANEL_W = 760    -- ширина вікна програми; воно притиснуте до правого краю на всю висоту
local VLC_W   = 880    -- розмір вікна VLC зліва від меню
local VLC_H   = 540
local ANIM_MS = ECO and 300 or 500   -- скільки триває ковзання (≈ windowsMove у appearance.lua)

-- Внутрішні відступи терміналу (зверху справа знизу зліва): текст не заходить
-- під заокруглені кути екрана й рамку — нічого не обрізається.
local PADDING = "12 22 12 14"

-- ───────────── ПРОГРАМИ ПАНЕЛІ ─────────────
-- id → клас вікна (регулярний вираз Hyprland). Запуск — hypr/comms-launch.sh <id>.
-- Щоб додати програму: 1) рядок сюди; 2) гілка в comms-launch.sh;
--                      3) пункт у waybar/comms.jsonc (on-click: CommsShow("<id>")).
local LAUNCH = HOME .. "/.config/hypr/comms-launch.sh "
local APPS = {
    -- id        = { re = клас для правил Hyprland (regex),     pat = той самий клас для пошуку (шаблон Lua) }
    nchat     = { re = "comms-nchat",                         pat = "^comms%-nchat$" },        -- Telegram у терміналі
    mail      = { re = "comms-mail",                          pat = "^comms%-mail$" },         -- пошта Gmail + ukr.net (aerc)
    discord   = { re = "comms-discord",                       pat = "^comms%-discord$" },      -- Discord у терміналі (endcord)
}

local DEFAULT  = "nchat"
local WS       = "comms"                          -- схованка special:comms (ніколи не показується)
local STATE    = RUN .. "/comms-active"           -- яка програма зараз відкрита (для підсвічування іконки)
local ADD_FLAG = RUN .. "/nchat-add-account"      -- прапорець «додати акаунт» (читає nchat/comms-run.sh)
local MENU_BIN = HOME .. "/.local/bin/waybar-comms" -- той самий waybar під іншим іменем: сигнали основної панелі (pkill -x waybar) його не чіпають
local HOLES    = RUN .. "/comms-holes"            -- де кліки НЕ закривають панель (читає desk — ловець кліків)
local SIGNAL   = 8                                -- сигнал оновлення іконок (= "signal" у comms.jsonc)

-- ───────────── ПРАВИЛА ─────────────
-- Вікна програм одразу народжуються плаваючими в схованці (правило не пускає
-- вікно за край екрана — за край його ставить window.open нижче)
for id, app in pairs(APPS) do
    hl.window_rule({
        name      = "comms-" .. id,
        match     = { class = "^" .. app.re .. "$" },
        float     = true,
        workspace = "special:" .. WS .. " silent",
        size      = PANEL_W .. " monitor_h",
        move      = "monitor_w-" .. PANEL_W .. " 0",
    })
end

-- ───────────── СТАН ─────────────
local current    = DEFAULT   -- яка програма висунута / буде висунута SUPER+M
local hide_gen   = 0         -- номер «приховання»: новий показ скасовує незавершене приховання
local media_pid  = nil       -- pid VLC, відкритого з чату (див. CommsMedia)
local media_wait = 0         -- до якого часу (os.time) чекаємо на вікно VLC
local menu_shown = nil

local shown      = false     -- панель висунута (або висувається)

local sel = P.sel

local function is_open() return shown end

-- Вікно програми з id (або nil)
local function app_window(id)
    local app = APPS[id]
    if not app then return nil end
    for _, w in ipairs(hl.get_windows() or {}) do
        if (w.class or ""):match(app.pat) then return w end
    end
end

local function media_window()
    if not media_pid then return nil end
    for _, w in ipairs(hl.get_windows() or {}) do
        if w.pid == media_pid then return w end
    end
end

-- Поставити вікно: x, y — від лівого верхнього кута монітора. Зміна позиції анімується.
local function place(w, x, y, width, height)
    local m = hl.get_active_monitor()
    if not (w and m) then return end
    if width then hl.dispatch(hl.dsp.window.resize({ x = width, y = height, window = sel(w) })) end
    hl.dispatch(hl.dsp.window.move({ x = m.x + x, y = m.y + y, window = sel(w) }))
end

-- Позиції: на екрані / за правим краєм
local function app_on(w)
    local m = hl.get_active_monitor()
    place(w, m.width - PANEL_W, 0, PANEL_W, m.height)
end
local function vlc_on(w)
    local m = hl.get_active_monitor()
    place(w, m.width - PANEL_W - GAP - MENU_W - GAP - VLC_W, math.floor((m.height - VLC_H) / 2), VLC_W, VLC_H)
end
local function off(w, y)
    local m = hl.get_active_monitor()
    place(w, m.width + 20, y or 0)
end
local function vlc_y()
    local m = hl.get_active_monitor()
    return math.floor(((m and m.height or 1080) - VLC_H) / 2)
end

-- Ловець кліків (у програмі desk): прозорий шар на весь екран, клік по ньому ховає панель.
-- У ньому «дірки» — там кліки проходять до чату, VLC і меню. Координати від кута монітора.
local function write_holes()
    local m = hl.get_active_monitor()
    if not m then return end
    local x = m.width - PANEL_W - GAP - MENU_W - GAP          -- меню + проміжок + панель
    local lines = { string.format("%d 0 %d %d", x, m.width - x, m.height) }
    local v = media_window()
    if v and v.at and v.size then                             -- VLC — там, де він зараз стоїть
        lines[#lines + 1] = string.format("%d %d %d %d", v.at.x - m.x, v.at.y - m.y, v.size.x, v.size.y)
    end
    local f = io.open(HOLES, "w")
    if f then f:write(table.concat(lines, "\n")); f:close() end
end

-- Оновити дірки, коли панель уже показана (VLC з'явився / переїхав)
local function refresh_holes()
    if not menu_shown then return end
    write_holes()
    hl.exec_cmd("pkill -USR1 -x desk")
end

-- Меню й підсвічування іконок
local function menu(show, force)
    if show then write_holes() end
    local f = io.open(STATE, "w")
    if f then f:write(show and current or ""); f:close() end
    local cmd = "pkill -RTMIN+" .. SIGNAL .. " -x waybar-comms"
    if show ~= menu_shown or force then
        menu_shown = show
        cmd = (show and "pkill -USR1" or "pkill -USR2") .. " -x waybar-comms; " .. cmd   -- USR1 = показати, USR2 = сховати
        -- desk: USR1 — панель висунута (гарячий край вимкнено, ловець кліків увімкнено), USR2 — навпаки
        cmd = cmd .. "; pkill " .. (show and "-USR1" or "-USR2") .. " -x desk"
    end
    hl.exec_cmd(cmd)
end

-- Усі вікна панелі (програми + VLC)
local function panel_windows()
    local list = {}
    for app in pairs(APPS) do
        local w = app_window(app)
        if w then list[#list + 1] = w end
    end
    local v = media_window()
    if v then list[#list + 1] = v end
    return list
end

local function is_panel(w)
    if not w then return false end
    if media_pid and w.pid == media_pid then return true end
    return (w.class or ""):match("^comms%-") ~= nil
end

-- Коли ковзання скінчилось: усе, що не має бути на екрані, — назад у схованку
local function tidy(gen)
    hl.timer(function ()
        if gen ~= hide_gen then return end
        for app in pairs(APPS) do
            local w = app_window(app)
            if w and not (shown and app == current) and P.out(w) then P.stash(w, WS) end
        end
        local v = media_window()
        if v and not shown and P.out(v) then P.stash(v, WS) end
        if not shown and WaybarPanelLeaving then WaybarPanelLeaving(WS, nil) end   -- доїхали — більше не «ховається»
    end, { timeout = ANIM_MS + 50, type = "oneshot" })
end

-- Висунути вікно: зі схованки на поточний стіл (воно там за краєм) — і ковзнути на місце
local function slide_in(w, place_fn)
    P.bring(w)
    place_fn(w)
end

-- ───────────── ПОКАЗ / ПРИХОВАННЯ ─────────────
local function show(id)
    current = id
    shown = true
    hide_gen = hide_gen + 1                     -- скасувати незавершене приховання
    P.close_special()                           -- сховок SUPER+S закрити — інакше панель під ним
    if WaybarPanelLeaving then WaybarPanelLeaving(WS, false) end  -- панель на екрані

    for app in pairs(APPS) do                   -- інші програми — за край
        local w = app_window(app)
        if w and app ~= id and P.out(w) then off(w) end
    end

    local w = app_window(id)
    if w then
        slide_in(w, app_on)
        hl.dispatch(hl.dsp.focus({ window = sel(w) }))
    else
        hl.exec_cmd(LAUNCH .. id)               -- ще не запущена — window.open висуне її
    end
    local v = media_window()
    if v then slide_in(v, vlc_on) end

    tidy(hide_gen)
    menu(true, true)                            -- меню одразу, разом з панеллю (і сигнал завжди — на випадок розсинхрону)

    -- Страховка: зрідка Hyprland переносить вікно на стіл, але не рушає його з-за краю.
    -- Якщо за 150 мс воно ще там — висунути ще раз.
    local gen = hide_gen
    hl.timer(function ()
        if gen ~= hide_gen or not shown then return end
        local x = app_window(current)
        local m = hl.get_active_monitor()
        if x and m and x.at and x.at.x >= m.x + m.width then slide_in(x, app_on) end
    end, { timeout = 150, type = "oneshot" })
end

local function hide()
    shown = false
    menu(false)
    local addrs = {}
    for _, w in ipairs(panel_windows()) do
        addrs[w.address] = true
        if P.out(w) then
            if media_pid and w.pid == media_pid then off(w, vlc_y()) else off(w) end
        end
    end
    if media_window() then
        hl.exec_cmd("playerctl -p vlc pause")   -- сховали — пауза, нічого не пропустиш
    end
    hide_gen = hide_gen + 1
    if WaybarPanelLeaving then WaybarPanelLeaving(WS, true, addrs) end   -- waybar — одразу, не чекаючи кінця ковзання
    tidy(hide_gen)                              -- коли вікна доїхали — у схованку
end

-- ───────────── ПОДІЇ ─────────────
-- Нове вікно програми панелі: правило поставило його в схованку на екрані —
-- переставити за край; якщо панель висунута саме для неї — виїхати.
local function app_opened(w)
    local id
    for app in pairs(APPS) do
        if (w.class or ""):match(APPS[app].pat) then id = app end
    end
    if not id then return false end
    off(w)
    local addr = w.address
    hl.timer(function ()
        local x = P.fresh(addr)
        if x and shown and id == current then
            slide_in(x, app_on)
            hl.dispatch(hl.dsp.focus({ window = sel(x) }))
        end
    end, { timeout = 60, type = "oneshot" })
    return true
end

-- Нове вікно: VLC, запущений з чату, переїжджає в панель зліва від меню.
-- Будь-яке інше вікно (посилання з чату відкрило браузер тощо) — ховає панель,
-- щоб ти одразу його бачив.
hl.on("window.open", function (w)
    if not w then return end
    if app_opened(w) then return end
    local is_media = (media_pid and w.pid == media_pid)
        or (os.time() < media_wait and (w.class or ""):lower():match("vlc"))
    if not is_media then
        -- Ховати лише через 300 мс і лише якщо вікно ще живе: деякі програми
        -- (напр. Vimium C у Firefox) відкривають службове вікно на мить і закривають
        if shown then
            local addr = w.address
            hl.timer(function ()
                local x = P.fresh(addr)
                if shown and x and not is_panel(x) and P.out(x) then hide()
                elseif shown and not x then                  -- вікно вже зникло — повернути фокус панелі
                    local p = app_window(current)
                    if p then hl.dispatch(hl.dsp.focus({ window = sel(p) })) end
                end
            end, { timeout = 300, type = "oneshot" })
        end
        return
    end
    media_pid  = w.pid
    media_wait = 0
    local maddr = w.address
    hl.timer(function ()
        local v = media_window() or P.fresh(maddr)
        if not v then return end                -- VLC уже закрили
        hl.dispatch(hl.dsp.window.float({ action = "enable", window = sel(v) }))
        off(v, vlc_y())
        if is_open() then
            slide_in(v, vlc_on)
            hl.dispatch(hl.dsp.focus({ window = sel(v) }))
        else
            P.stash(v, WS)
        end
    end, { timeout = 60, type = "oneshot" })
    hl.timer(function ()                        -- VLC сам змінює розмір після завантаження відео — поправити ще раз
        local v = media_window()
        if v and is_open() and shown then vlc_on(v) end
        hl.timer(refresh_holes, { timeout = ANIM_MS, type = "oneshot" })   -- VLC доїхав — оновити дірки
    end, { timeout = 900, type = "oneshot" })
end)

-- Програма, що вже відкрита (напр. браузер), просить уваги — посилання з чату
-- відкрилось у ній: сховати панель і перейти до неї
hl.on("window.urgent", function (w)
    if not (shown and w) or is_panel(w) then return end
    local a = hl.get_active_window()
    if not is_panel(a) then return end          -- просили уваги не через тебе в чаті — не чіпати
    hide()
    hl.dispatch(hl.dsp.focus({ window = sel(w) }))
end)

-- Відкрили сховок SUPER+S — панель ховається (інакше лишилась би під ним)
hl.on("workspace.special_active", function ()
    hl.timer(function ()
        if shown and hl.get_active_special_workspace() then hide() end
    end, { timeout = 40, type = "oneshot" })
end)

-- Закрили VLC — прибрати його дірку; вийшли з програми панелі — скинути стан
hl.on("window.close", function ()
    hl.timer(function ()
        refresh_holes()
        local w = app_window(current)
        if shown and not P.out(w) then
            shown = false
            if menu_shown then menu(false) end
            if WaybarPanelLeaving then WaybarPanelLeaving(WS, nil) end
        end
    end, { timeout = 60, type = "oneshot" })
end)

-- Запуск помічників панелі: меню (waybar-comms) і гарячий край з ловцем кліків (desk, через desk.sh).
-- pgrep — щоб не запустити двічі. Обидва стартують схованими.
local function start_helpers()
    hl.exec_cmd("pgrep -x waybar-comms >/dev/null || GTK_MODULES=$HOME/.local/lib/waybar-islands.so " .. MENU_BIN
        .. " -c " .. HOME .. "/.config/waybar/comms.jsonc -s " .. HOME .. "/.config/waybar/comms.css")
    hl.exec_cmd("pgrep -x desk >/dev/null || " .. HOME .. "/.config/hypr/desk.sh")
    hl.timer(function () menu(is_open()) end, { timeout = 1500, type = "oneshot" })
end

-- Після перечитування конфігу стан скинуто (shown = false) — повернути
-- висунуті вікна в схованку (зокрема ті, що лишились від старої версії на спецстолі)
local function restash()
    for _, w in ipairs(panel_windows()) do
        if P.out(w) then off(w); P.stash(w, WS) end
    end
end
hl.on("config.reloaded", function () hl.timer(restash, { timeout = 200, type = "oneshot" }) end)

-- Чати, що працюють у фоні з самого входу (сховані в схованці) — інакше
-- сповіщення з Telegram / Discord приходять лише після першого SUPER+M.
-- Лише при вході: якщо закрив чат сам (Ctrl+Q), SUPER+Shift+R його не воскресить.
local BACKGROUND = { "nchat", "discord" }
local function start_chats()
    for _, id in ipairs(BACKGROUND) do
        if not app_window(id) then hl.exec_cmd(LAUNCH .. id) end
    end
end

-- Після входу в систему: конфіг читається ще ДО того, як Hyprland підняв екран,
-- і запущені тоді програми одразу падають — тому стартуємо, коли Hyprland готовий.
hl.on("hyprland.start", start_helpers)
hl.on("hyprland.start", function () hl.timer(start_chats, { timeout = 3000, type = "oneshot" }) end)   -- після мережі й сповіщень
-- Після перечитування конфігу (SUPER+Shift+R) — перезапустити, якщо їх немає
hl.on("config.reloaded", start_helpers)


-- ───────────── ГЛОБАЛЬНІ ФУНКЦІЇ (клавіші, меню, скрипти) ─────────────

-- SUPER+M: висунути останню програму / сховати панель
function CommsToggle()
    if is_open() and shown then hide() else show(current) end
end

-- Клік по іконці в меню: показати програму; клік по вже відкритій — сховати панель
function CommsShow(id)
    if not APPS[id] then return end
    if is_open() and id == current then hide() else show(id) end
end

-- Рядок «Додати акаунт» у меню: kind = "nchat" (Telegram / WhatsApp), "mail" (ще одна
-- скринька), "discord" (менеджер профілів endcord: додати / вибрати юзера).
-- Програма закривається, у тому самому вікні її comms-run.sh бачить прапорець і
-- запускає додавання, потім знову відкриває програму.
local ADD = {
    nchat   = { flag = ADD_FLAG,                     proc = "nchat"   },
    mail    = { flag = RUN .. "/mail-add-account",    proc = "aerc"    },
    discord = { flag = RUN .. "/discord-add-account", proc = "endcord" },
}
function CommsAddAccount(kind)
    kind = ADD[kind] and kind or "nchat"
    local a = ADD[kind]
    hl.exec_cmd("touch " .. a.flag .. "; pkill -TERM -x " .. a.proc)
    show(kind)
end

-- Клік повз панель (ловець кліків у desk)
function CommsHide()
    if is_open() and shown then hide() end
end

-- Викликає nchat/open-media.sh: «зараз відкриється VLC з таким pid — постав його в панель»
function CommsMedia(pid)
    media_pid  = tonumber(pid)
    media_wait = os.time() + 8
end
