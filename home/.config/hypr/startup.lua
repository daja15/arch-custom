-- ╭──────────────────────────────────────────────────────────────╮
-- │  СТАРТОВІ ВІКНА                                              │
-- │  • scratchpad (SUPER+S) — група-вкладки з двох терміналів:   │
-- │    Claude Code + звичайний термінал                          │
-- ╰──────────────────────────────────────────────────────────────╯

local SCRATCH = "magic"                 -- назва спецстолу, той самий, що SUPER+S
local CLAUDE  = "scratch-claude"        -- клас вікна kitty з Claude Code
local TERM    = "scratch-term"          -- клас вікна kitty зі звичайним терміналом

-- Знайти вікно за класом (точний збіг)
local function find(class)
    for _, w in ipairs(hl.get_windows()) do
        if w.class == class then return w end
    end
end

-- Обидва термінали є → склеїти в одну групу (вкладки, SUPER+Alt+Tab — перемикання)
local function group_up()
    local c, t = find(CLAUDE), find(TERM)
    if not (c and t) then return end                  -- ще не всі відкрились
    if c.group and t.group and c.group == t.group then return end
    if not c.group then return end                    -- група з’являється з правилом group=set
    if t.group then return end                        -- термінал уже в якійсь іншій групі — не чіпати
    c.group:add(t)                                    -- додати термінал вкладкою до Claude
end

hl.on("window.open", function (w)
    if not w then return end
    if w.class == CLAUDE or w.class == TERM then
        hl.timer(group_up, { timeout = 80, type = "oneshot" })   -- дати вікну «сісти» на місце
    end
end)

-- Запуск (глобальна — щоб можна було повторити: hyprctl eval "StartupWindows()")
function StartupWindows()
    if not find(CLAUDE) then
        -- Окремий kitty (без --single-instance): інакше вікно відкриває вже запущений kitty,
        -- і правила запуску губляться — Claude опинявся на столі 1. Стіл і групу задає rules.lua за класом.
        hl.exec_cmd("kitty --class " .. CLAUDE .. " -d ~ zsh -ic 'claude; exec zsh'")  -- Claude Code; після виходу лишається shell
    end
    if not find(TERM) then
        hl.exec_cmd("kitty --class " .. TERM .. " -d ~")                          -- звичайний термінал
    end
end

hl.on("hyprland.start", function ()
    hl.timer(StartupWindows, { timeout = 300, type = "oneshot" })              -- трохи після старту (монітор уже готовий)
end)
