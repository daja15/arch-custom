-- ╭──────────────────────────────────────────────────────────────╮
-- │  ГРАФІЧНИЙ ПЛАНШЕТ (Wacom та інші; кабель або Bluetooth)     │
-- │                                                              │
-- │  Ядро (драйвер wacom) + libinput уже розуміють планшет:      │
-- │  перо = курсор, натиск і нахил отримують програми (Krita,    │
-- │  GIMP, Inkscape…). Тут — лише налаштування:                  │
-- │  ~/.config/hypr/tablet-settings.lua — його пише віджет       │
-- │  «Планшет» на столі (hypr/src/tablet.c) і викликає           │
-- │  TabletApply(). Вручну: змінити файл →                       │
-- │      hyprctl eval "TabletApply()"                            │
-- ╰──────────────────────────────────────────────────────────────╯
local FILE = os.getenv("HOME") .. "/.config/hypr/tablet-settings.lua"

-- Прочитати налаштування (файл — Lua-таблиця; виконується без доступу до чогось, лише дані)
local function read()
    local f = io.open(FILE, "r")
    if not f then return {} end
    local src = f:read("a")
    f:close()
    local chunk = load(src, "tablet-settings", "t", {})
    if not chunk then return {} end
    local ok, t = pcall(chunk)
    return (ok and type(t) == "table") and t or {}
end

function TabletApply()
    local s = read()
    local p = tonumber(s.pressure) or 0          -- −1 м'якше … 1 жорсткіше
    hl.config({
        input = {
            tablet = {
                output         = s.output or "",          -- екран ("" — усі)
                left_handed    = s.left == true,          -- лівша: поворот на 180°
                relative_input = s.relative == true,      -- як миша
                active_area_size     = { tonumber(s.area_w) or 0, tonumber(s.area_h) or 0 },   -- мм; 0 — уся поверхня
                active_area_position = { tonumber(s.area_x) or 0, tonumber(s.area_y) or 0 },
            },
            tablettool = {
                -- натиск: м'якше — повний натиск настає раніше (верхня межа нижче);
                -- жорсткіше — найлегший дотик (до 5%) ще не малює: від випадкових штрихів
                pressure_range_min = p > 0 and p * 0.05 or 0,   -- не більше 5%: інакше легкі штрихи зникають
                pressure_range_max = p < 0 and 1 + p * 0.5 or 1,
            },
        },
    })
end

TabletApply()
