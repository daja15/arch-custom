-- ╭──────────────────────────────────────────────────────────────╮
-- │  ПОВНИЙ ЕКРАН У «ПОЛОВИНІ»                                   │
-- │  Вікно просить «на весь екран» (відео у Firefox, F11,        │
-- │  SUPER+F), а на робочому столі поруч є інші вікна → воно     │
-- │  заповнює лише СВОЮ частину: програма думає, що вона на      │
-- │  весь екран (ховає свої панелі), а Hyprland лишає вікно на   │
-- │  місці.                                                      │
-- │  Щоб відео було на весь монітор — постав вікно на робочий    │
-- │  стіл саме (тоді повний екран справжній).                    │
-- │  SUPER+D (розгорнути з панеллю) не чіпаємо.                  │
-- │  X11-програми (XWayland: Resolve…) — завжди справжній.       │
-- ╰──────────────────────────────────────────────────────────────╯
local FULL = 2      -- стан «на весь екран» (1 — розгорнуте, 0 — звичайне)

local function tiled_on(ws)       -- скільки звичайних (не плаваючих) вікон на столі
    local n = 0
    for _, x in ipairs(hl.get_workspace_windows(ws) or {}) do
        if x.mapped and not x.floating then n = n + 1 end
    end
    return n
end

hl.on("window.fullscreen", function (w)
    if not w or w.fullscreen ~= FULL or w.floating then return end
    -- X11-програми (через XWayland, напр. DaVinci Resolve) у «фейковому»
    -- повному екрані малюють себе на весь монітор, а кліки отримують у
    -- координатах половинки → курсор натискає не туди. Їм — лише справжній.
    if w.xwayland then return end
    if tiled_on(w.workspace) > 1 then
        -- Hyprland: звичайне вікно; програма: на весь екран
        hl.dispatch(hl.dsp.window.fullscreen_state({ internal = 0, client = FULL, window = "address:" .. w.address }))
    end
end)
