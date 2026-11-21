-- ╭──────────────────────────────────────────────────────────────╮
-- │  АВТОПОКАЗ WAYBAR                                            │
-- │  Панель з'являється лише коли:                               │
-- │    • її відкрито клавішею SUPER, або                         │
-- │    • курсор біля верхнього краю екрана (гарячий край), або   │
-- │    • відкрито rofi, або                                      │
-- │    • на поточному робочому столі немає вікон.                │
-- │  Працює на подіях Hyprland — жодних фонових скриптів         │
-- │  і опитувань, у спокої 0 навантаження.                       │
-- │  Waybar керується сигналами:                                 │
-- │    SIGUSR1 = показати, SIGUSR2 = сховати (див. config.jsonc) │
-- ╰──────────────────────────────────────────────────────────────╯

local SHOW_WITH_ROFI  = true   -- показувати панель, коли відкрито rofi
local SHOW_WHEN_EMPTY = true   -- показувати панель на порожньому столі
local DELAY_MS        = 40     -- затримка перевірки після події (щоб Hyprland встиг оновити стан)

local shown  = nil     -- поточний стан панелі: nil = невідомо (після перезавантаження конфігу перша перевірка синхронізує)
local pinned = false   -- SUPER+B: закріпити панель (показувати завжди)
local super_held = false -- панель відкрито клавішею SUPER (див. keyBinds.lua)
local hovered    = false -- панель відкрито гарячим краєм (курсор біля верху екрана)

-- Чи відкрито rofi
local function rofi_open()
    for _, l in ipairs(hl.get_layers() or {}) do
        if l.namespace == "rofi" and l.mapped ~= false then return true end
    end
    return false
end

RofiIsOpen = rofi_open   -- глобально: використовується в keyBinds.lua

-- Бокові панелі (музика, зв'язок) — вікна, закріплені на поточному столі.
-- Висунута панель = стіл не порожній. Панель, що ховається: її вікна ще
-- виїжджають за край — їх уже не рахуємо, інакше waybar з'явився б аж після
-- анімації. Див. WaybarPanelLeaving.
local panel_open = {}   -- назва панелі → true (висунута)
local leaving    = {}   -- назва панелі → { [адреса вікна] = true } (ховається)

-- Чи порожній поточний робочий стіл
local function workspace_empty()
    if next(panel_open) then return false end                         -- висунута бокова панель
    local special = hl.get_active_special_workspace()
    if special and (special.windows or 0) > 0 then return false end  -- відкритий scratchpad з вікнами
    local m = hl.get_active_monitor()
    local ws = m and m.active_workspace
    if ws == nil then return true end
    for _, w in ipairs(hl.get_workspace_windows(ws) or {}) do
        local gone = false
        for _, set in pairs(leaving) do
            if set[w.address] then gone = true end                    -- вікно панелі, що ховається
        end
        if not gone then return false end
    end
    return true
end

-- Надіслати сигнал, якщо стан змінився (force — надіслати в будь-якому разі).
-- Сигнали групуються: за SEND_MS надсилається лише ОСТАННІЙ стан одним процесом,
-- щоб «показати» і «сховати», що йдуть впритул, не прийшли в неправильному порядку.
local SEND_MS = 1      -- лише щоб злити сигнали з однієї події в один (раніше 15 мс — давало помітне запізнення)
local send_queued = false
local function apply(want, force)
    if want == shown and not force then return end
    shown = want
    if send_queued then return end
    send_queued = true
    hl.timer(function ()
        send_queued = false
        hl.exec_cmd(shown and "pkill -USR1 -x waybar" or "pkill -USR2 -x waybar")
    end, { timeout = SEND_MS, type = "oneshot" })
end

-- Waybar лежить над rofi завдяки order = -1 у rules.lua, тож при відкритті rofi
-- поверх уже видимої панелі нічого не робимо (раніше її ховали й показували
-- знову, щоб перепідняти, — це ламало панель).
local function update(force)
    local want = pinned
        or super_held
        or hovered
        or (SHOW_WITH_ROFI  and rofi_open())
        or (SHOW_WHEN_EMPTY and workspace_empty())
    apply(want, force)
end

-- Перевірка ОДРАЗУ (панель з'являється без запізнення, разом з анімацією вікон)
-- і ще раз через DELAY_MS — на випадок, якщо в момент події вікно/шар ще в списку.
local function schedule()
    update()
    hl.timer(update, { timeout = DELAY_MS, type = "oneshot" })
end

-- Події, після яких стан міг змінитися
for _, ev in ipairs({
    "layer.opened", "layer.closed",                         -- rofi відкрито / закрито
    "window.open", "window.close", "window.destroy",        -- вікна з'явились / зникли
    "window.move_to_workspace",                             -- вікно перенесли на інший стіл
    "workspace.active", "workspace.special_active",         -- перемкнули стіл / scratchpad
    "monitor.focused",                                      -- фокус на інший монітор
}) do
    hl.on(ev, schedule)
end

-- Перша перевірка після старту waybar
hl.on("hyprland.start", function ()
    hl.timer(update, { timeout = 1500, type = "oneshot" })
end)
hl.on("config.reloaded", schedule)


-- ───────────── ГЛОБАЛЬНІ ФУНКЦІЇ ДЛЯ КЛАВІШ (keyBinds.lua) ─────────────

-- SUPER+B — закріпити / відкріпити панель
function WaybarTogglePin()
    pinned = not pinned
    update(true)
end

-- SUPER+Shift+B — перезапустити waybar (стартує схованим, тож скидаємо стан)
function WaybarRestart()
    shown = false
    hl.exec_cmd("pkill -x waybar; sleep 0.3; GTK_MODULES=$HOME/.local/lib/waybar-islands.so waybar")   -- islands: див. src/islands.c
    hl.timer(update, { timeout = 1200, type = "oneshot" })
end

-- Викликається з keyBinds.lua: відкрити / закрити панель клавішею SUPER
function WaybarSetSuper(open, quiet)
    super_held = open
    if quiet then return end   -- лише змінити стан (панель тримає rofi; сховається, коли він закриється)
    update(true)   -- одразу; сигнал надсилається завжди — це виправляє будь-який розсинхрон
end

-- Викликається з music.lua / comms.lua. name — назва панелі ("music", "comms").
--   on = false — панель висувається;
--   on = true  — почала ховатися, addrs = { [адреса вікна] = true } її вікон;
--   on = nil   — сховалась повністю (вікна вже в схованці).
function WaybarPanelLeaving(name, on, addrs)
    panel_open[name] = (on == false) or nil
    leaving[name]    = on and (addrs or {}) or nil
    update(true)   -- одразу: панель з'являється разом з першим кадром ковзання
end

-- Чи відкрита панель клавішею SUPER
function WaybarSuperOpen()
    return super_held
end

-- ───────────── ГАРЯЧИЙ КРАЙ ЗВЕРХУ ─────────────
-- Невидима смужка вгорі екрана (у програмі desk) викликає WaybarHover()
-- щойно курсор торкнеться верхнього краю. Поки панель відкрита краєм, раз на
-- HOVER_POLL_MS перевіряємо, де курсор; коли він пішов нижче панелі й не
-- повертається HOVER_LEAVE_MS — ховаємо. Таймер працює лише в цей час.
local HOVER_ZONE     = 8 + 36 + 28   -- висота «зони панелі» від верху монітора: відступ + висота waybar + запас (px)
local HOVER_POLL_MS  = 80            -- інтервал перевірки курсора (мс)
local HOVER_LEAVE_MS = 350           -- скільки курсор має бути поза зоною, щоб панель сховалась (мс)
local hover_timer, hover_out = nil, 0

local function hover_poll()
    local p = hl.get_cursor_pos()
    local m = hl.get_monitor_at_cursor()
    local inside = p and m and (p.y - m.y) <= HOVER_ZONE
    if inside or p == nil then
        hover_out = 0
        return
    end
    hover_out = hover_out + HOVER_POLL_MS
    if hover_out >= HOVER_LEAVE_MS then
        hovered = false
        if hover_timer then hover_timer:set_enabled(false) end
        update(true)
    end
end

function WaybarHover()
    hover_out = 0
    if not hovered then
        hovered = true
        update(true)
    end
    if hover_timer then
        hover_timer:set_enabled(true)
    else
        hover_timer = hl.timer(hover_poll, { timeout = HOVER_POLL_MS, type = "repeat" })
    end
end

-- Діагностика: hyprctl eval 'WaybarDebug("/tmp/файл")' → причини, чому панель видно
function WaybarDebug(path)
    local f = io.open(path, "w")
    f:write(string.format("shown=%s pinned=%s super=%s hover=%s rofi=%s empty=%s",
        tostring(shown), tostring(pinned), tostring(super_held), tostring(hovered), tostring(rofi_open()), tostring(workspace_empty())))
    f:close()
end
