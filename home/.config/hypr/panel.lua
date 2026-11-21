-- ╭──────────────────────────────────────────────────────────────╮
-- │  СПІЛЬНЕ ДЛЯ БОКОВИХ ПАНЕЛЕЙ (музика, зв'язок)               │
-- │                                                              │
-- │  Панель — НЕ окремий стіл. Сховані вікна лежать у «схованці» │
-- │  (спецстіл, який ніколи не показується). Висунути =          │
-- │  перенести вікно на твій поточний стіл (за краєм екрана, тож │
-- │  цього не видно), закріпити (pin — їде з тобою на інші столи)│
-- │  і виїхати з краю. Сховати = заїхати за край і повернутись у │
-- │  схованку. Тому нові вікна (посилання з чату, програми)      │
-- │  відкриваються на звичайному столі, а не «в панелі».         │
-- ╰──────────────────────────────────────────────────────────────╯

local P = {}

function P.sel(w) return "address:" .. w.address end

-- Свіжий стан вікна за адресою (поля вікна — знімок на момент запиту)
function P.fresh(addr)
    for _, w in ipairs(hl.get_windows() or {}) do
        if w.address == addr then return w end
    end
end

-- Вікно зараз висунуте (на звичайному столі), а не в схованці
function P.out(w)
    return w ~= nil and w.workspace ~= nil and not w.workspace.special
end

-- Звичайний (не спец) стіл поточного монітора
local function current_ws()
    local m = hl.get_active_monitor()
    return m and m.active_workspace
end

-- Закрити відкритий спецстіл (сховок SUPER+S), щоб панель не опинилась під ним
function P.close_special()
    local sp = hl.get_active_special_workspace()
    if sp then hl.dispatch(hl.dsp.workspace.toggle_special((sp.name:gsub("^special:", "")))) end
end

-- Зі схованки на поточний стіл + закріпити. Вікно має вже стояти за краєм.
function P.bring(w)
    if not w then return end
    local ws = current_ws()
    if (not P.out(w)) and ws then
        hl.dispatch(hl.dsp.window.move({ workspace = ws.id, follow = false, window = P.sel(w) }))
    end
    w = P.fresh(w.address) or w
    if not w.pinned then hl.dispatch(hl.dsp.window.pin({ action = "enable", window = P.sel(w) })) end
end

-- Назад у схованку special:<name> (відкріпити — закріплене вікно не переноситься)
function P.stash(w, name)
    if not w then return end
    if w.pinned then hl.dispatch(hl.dsp.window.pin({ action = "disable", window = P.sel(w) })) end
    if not (w.workspace and w.workspace.name == "special:" .. name) then
        hl.dispatch(hl.dsp.window.move({ workspace = "special:" .. name, follow = false, window = P.sel(w) }))
    end
end

return P
