-- ╭──────────────────────────────────────────────────────────────╮
-- │  КОМБІНАЦІЇ КЛАВІШ                                           │
-- │  SUPER = клавіша Windows                                     │
-- │  https://wiki.hypr.land/configuring/core/binds/              │
-- ╰──────────────────────────────────────────────────────────────╯

local mod = "SUPER"
local rep = { repeating = true }                       -- повторювати, поки клавіша затиснута
local media = { locked = true, repeating = true }      -- працює навіть на екрані блокування


-- ───────────── SUPER: WAYBAR + ROFI ─────────────
-- SUPER затиснуто                 → waybar видно; відпустив — сховався (без анімації)
-- SUPER (коли відкрито rofi)      → коротке натискання закриває rofi
-- SUPER (без комбінації)          → ховає бокові панелі: чати (SUPER+M) і музику (SUPER+A)
-- SUPER+R                         → відкрити rofi (waybar тримається, поки rofi відкритий);
--                                   якщо rofi відкритий — закрити

local TAP_MS = 600            -- максимальна тривалість «простого натискання» SUPER (мс)
                              -- (довше = вважаємо, що ти щось робив, напр. тягнув вікно мишкою)

local rofi_pending = false    -- rofi щойно запущено, але вікно ще не з'явилось
local combo_used   = false    -- під час утримання SUPER була виконана комбінація
local super_down   = false    -- SUPER зараз затиснуто
local tap_expired  = false    -- SUPER тримали довше за TAP_MS
local close_on_tap = false    -- на момент натискання був відкритий rofi → закрити при відпусканні

-- Закрити rofi, якщо він відкритий або саме запускається
function RofiClose()
    if rofi_pending or RofiIsOpen() then hl.exec_cmd("pkill -x rofi") end
end

-- Закрити все: rofi і waybar (waybar лишиться лише на порожньому столі / якщо закріплена SUPER+B)
local function close_all()
    RofiClose()
    WaybarSetSuper(false)
end

-- Позначити, що SUPER використано в комбінації (викликає обгортка bind нижче)
function SuperComboUsed()
    combo_used = true
end

-- Відкрити універсальне меню (якщо вже відкрите — нічого не робить)
local function rofi_open_launcher()
    if rofi_pending or RofiIsOpen() then return end
    rofi_pending = true
    hl.exec_cmd(launcher)
    hl.timer(function () rofi_pending = false end, { timeout = 700, type = "oneshot" })
end


local super_opts = {
    ignore_mods   = true,   -- спрацьовує незалежно від інших модифікаторів
    non_consuming = true,   -- клавіша SUPER далі передається системі (комбінації працюють)
}

-- Відпускання SUPER (викликається один раз за натискання)
local function on_super_release()
    if not super_down then return end      -- вже оброблено
    super_down = false
    if close_on_tap and not combo_used and not tap_expired then RofiClose() end   -- коротке натискання — закрити rofi
    -- SUPER без комбінації — сховати бокові панелі (чати, музика). Саме при відпусканні:
    -- при натисканні SUPER+M / SUPER+A спершу сховали б панель, а потім знову висунули.
    if not combo_used then
        if CommsHide then CommsHide() end
        if MusicHide then MusicHide() end
    end
    -- Сховати панель. Якщо rofi відкритий або запускається (SUPER+R) — її тримає rofi,
    -- сховається разом із ним (без мерехтіння).
    WaybarSetSuper(false, rofi_pending or RofiIsOpen())
end

-- Стеження за відпусканням: Hyprland НЕ викликає release-прив'язку SUPER після
-- комбінації (SUPER+1 тощо), тому поки SUPER затиснуто, раз на POLL_MS перевіряємо
-- клавішу напряму. Таймер працює лише під час утримання SUPER — у спокої 0 навантаження.
local POLL_MS     = 40                     -- інтервал перевірки (мс)
local key_seen    = false                  -- is_key_down хоч раз підтвердив, що SUPER затиснуто
local poll_timer  = nil
local poll_ticks  = 0                      -- скільки перевірок пройшло з натискання

local function super_is_down()
    return hl.is_key_down("Super_L") or hl.is_key_down("Super_R")
end

local function poll()
    if not super_down then
        if poll_timer then poll_timer:set_enabled(false) end
        return
    end
    poll_ticks = poll_ticks + 1
    if super_is_down() then
        key_seen = true
    elseif key_seen                        -- раніше бачили натиснутою, тепер відпущено
        or (combo_used and poll_ticks * POLL_MS >= 400) then   -- запобіжник: стан клавіші не читається
        if poll_timer then poll_timer:set_enabled(false) end
        on_super_release()
    end
end

-- Натискання SUPER
local function on_super_press()
    if super_down then return end          -- автоповтор клавіші — ігноруємо
    super_down, combo_used, tap_expired, key_seen, poll_ticks = true, false, false, false, 0
    close_on_tap = rofi_pending or RofiIsOpen()
    WaybarSetSuper(true)                   -- панель видно, поки SUPER затиснуто
    hl.timer(function () if super_down then tap_expired = true end end,
             { timeout = TAP_MS, type = "oneshot" })
    if poll_timer then
        poll_timer:set_enabled(true)
    else
        poll_timer = hl.timer(poll, { timeout = POLL_MS, type = "repeat" })
    end
end

-- SUPER+R: відкрити waybar + rofi, або закрити все, якщо rofi вже відкритий
local function on_super_r()
    SuperComboUsed()
    if rofi_pending or RofiIsOpen() then
        close_all()
    else
        rofi_open_launcher()               -- панель уже видно (SUPER затиснуто); далі її тримає rofi
    end
end

hl.bind("SUPER_L", on_super_press, super_opts)                                                     -- SUPER (натискання)
hl.bind("SUPER_L", on_super_release, { ignore_mods = true, non_consuming = true, release = true }) -- SUPER (відпускання без комбінацій)
hl.bind(mod .. " + R", on_super_r)                                                                 -- SUPER+R — меню (waybar + rofi) / закрити

-- Для перевірки без клавіатури: hyprctl eval 'SuperTest("press")' тощо
function SuperTest(what)
    if what == "press" then on_super_press()
    elseif what == "release" then on_super_release()
    elseif what == "r" then on_super_r()
    elseif what == "combo" then SuperComboUsed() end
end

-- Усі інші SUPER-комбінації нижче: позначають «комбінацію» (панель, відкрита натисканням
-- SUPER, після відпускання сховається), закривають rofi, якщо він відкритий, і виконують свою дію.
local function bind(keys, dsp, opts)
    if opts and opts.mouse then return hl.bind(keys, dsp, opts) end   -- перетягування мишкою — без змін (його відсікає TAP_MS)
    return hl.bind(keys, function ()
        SuperComboUsed()
        RofiClose()
        if type(dsp) == "function" then dsp() else hl.dispatch(dsp) end
    end, opts)
end

-- ───────────── ПРОГРАМИ ─────────────
bind(mod .. " + T",       hl.dsp.exec_cmd(terminal))      -- SUPER+T — термінал (kitty)
bind(mod .. " + E",       hl.dsp.exec_cmd(fileManager))   -- SUPER+E — файловий менеджер (dolphin)
bind(mod .. " + W",       hl.dsp.exec_cmd(browser))       -- SUPER+W — браузер (firefox)
bind(mod .. " + L",       hl.dsp.exec_cmd(locker))                          -- SUPER+L — заблокувати екран

bind(mod .. " + B",       WaybarTogglePin)                                  -- SUPER+B — закріпити / відкріпити waybar (див. waybarAuto.lua)
bind(mod .. " + SHIFT + B", WaybarRestart)                                  -- SUPER+Shift+B — перезапустити waybar
bind(mod .. " + SHIFT + R", hl.dsp.reload_config())                         -- SUPER+Shift+R — перечитати конфіг Hyprland
bind(mod .. " + SHIFT + M", hl.dsp.exit())                                  -- SUPER+Shift+M — вийти з Hyprland


-- ───────────── ВІКНА ─────────────
bind(mod .. " + Q",       hl.dsp.window.close())                                   -- SUPER+Q — закрити вікно
bind(mod .. " + D",       hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle" })) -- SUPER+D — розгорнути (з панеллю)
bind(mod .. " + F",       hl.dsp.window.fullscreen({ action = "toggle" }))        -- SUPER+F — на весь екран
bind(mod .. " + V",       hl.dsp.window.float({ action = "toggle" }))             -- SUPER+V — плаваюче / в сітці
bind(mod .. " + C",       hl.dsp.window.center())                                  -- SUPER+C — центрувати плаваюче вікно
bind(mod .. " + P",       hl.dsp.window.pseudo())                                  -- SUPER+P — псевдо-тайлінг
bind(mod .. " + J",       hl.dsp.layout("togglesplit"))                            -- SUPER+J — змінити напрямок поділу
bind(mod .. " + G",       hl.dsp.group.toggle())                                   -- SUPER+G — згрупувати вікна у вкладки
bind(mod .. " + ALT + TAB", hl.dsp.group.next())                                   -- SUPER+Alt+Tab — наступна вкладка групи

-- Фокус: SUPER + стрілки
bind(mod .. " + left",  hl.dsp.focus({ direction = "left" }))    -- фокус ліворуч
bind(mod .. " + right", hl.dsp.focus({ direction = "right" }))   -- фокус праворуч
bind(mod .. " + up",    hl.dsp.focus({ direction = "up" }))      -- фокус вгору
bind(mod .. " + down",  hl.dsp.focus({ direction = "down" }))    -- фокус вниз

-- Переміщення: SUPER + SHIFT + стрілки
bind(mod .. " + SHIFT + left",  hl.dsp.window.move({ direction = "left" }))   -- посунути вікно ліворуч
bind(mod .. " + SHIFT + right", hl.dsp.window.move({ direction = "right" }))  -- посунути вікно праворуч
bind(mod .. " + SHIFT + up",    hl.dsp.window.move({ direction = "up" }))     -- посунути вікно вгору
bind(mod .. " + SHIFT + down",  hl.dsp.window.move({ direction = "down" }))   -- посунути вікно вниз

-- Розмір: SUPER + CTRL + стрілки (затиснути — плавно)
bind(mod .. " + CTRL + left",  hl.dsp.window.resize({ x = -40, y = 0,   relative = true }), rep) -- вужче
bind(mod .. " + CTRL + right", hl.dsp.window.resize({ x = 40,  y = 0,   relative = true }), rep) -- ширше
bind(mod .. " + CTRL + up",    hl.dsp.window.resize({ x = 0,   y = -40, relative = true }), rep) -- нижче
bind(mod .. " + CTRL + down",  hl.dsp.window.resize({ x = 0,   y = 40,  relative = true }), rep) -- вище

-- Мишка
bind(mod .. " + mouse:272", hl.dsp.window.drag(),   { mouse = true })   -- SUPER + ЛКМ — тягнути вікно
bind(mod .. " + mouse:273", hl.dsp.window.resize(), { mouse = true })   -- SUPER + ПКМ — змінити розмір


-- ───────────── РОБОЧІ СТОЛИ ─────────────
-- Перейти на стіл N «начисто»: сховати бокові панелі (чати, музика), закрити відкритий
-- спецстіл (сховок SUPER+S тощо) — і показати сам стіл N (rofi закриває bind()).
local function goto_ws(i)
    return function ()
        if CommsHide then CommsHide() end
        if MusicHide then MusicHide() end
        local sp = hl.get_active_special_workspace()
        if sp then hl.dispatch(hl.dsp.workspace.toggle_special((sp.name:gsub("^special:", "")))) end
        hl.dispatch(hl.dsp.focus({ workspace = i }))
    end
end

for i = 1, 10 do
    local key = i % 10   -- 10-й стіл — клавіша 0
    bind(mod .. " + " .. key,         goto_ws(i))                             -- SUPER+N — перейти на стіл N (сховавши панелі й спецстіл)
    bind(mod .. " + SHIFT + " .. key, hl.dsp.window.move({ workspace = i }))  -- SUPER+Shift+N — перенести вікно на стіл N
    bind(mod .. " + ALT + " .. key,   hl.dsp.window.move({ workspace = i, follow = false })) -- SUPER+Alt+N — відправити вікно на стіл N, а самому лишитися тут
end

bind(mod .. " + mouse_down", hl.dsp.focus({ workspace = "e+1" }))   -- SUPER + коліщатко вниз — наступний стіл
bind(mod .. " + mouse_up",   hl.dsp.focus({ workspace = "e-1" }))   -- SUPER + коліщатко вгору — попередній стіл
bind(mod .. " + ALT + right", hl.dsp.focus({ workspace = "e+1" }))  -- SUPER+Alt+→ — наступний стіл
bind(mod .. " + ALT + left",  hl.dsp.focus({ workspace = "e-1" }))  -- SUPER+Alt+← — попередній стіл

-- Scratchpad (схований стіл, що випливає поверх)
bind(mod .. " + S",         hl.dsp.workspace.toggle_special("magic"))             -- SUPER+S — показати / сховати scratchpad
bind(mod .. " + SHIFT + S", hl.dsp.window.move({ workspace = "special:magic" }))  -- SUPER+Shift+S — кинути вікно в scratchpad

-- Сповіщення: спливні — swaync, історія — віджет на робочому столі (desk.sh toggle notify)

-- Права панель «Зв'язок» (див. comms.lua)
bind(mod .. " + M", function () CommsToggle() end)   -- SUPER+M — відкрити / сховати панель месенджерів

-- Ліва панель «Музика» (див. music.lua)
bind(mod .. " + A", function () MusicToggle() end)   -- SUPER+A — відкрити / сховати музику (kew + пошук)
-- Ті самі дії, що й іконки праворуч від панелі (~/.local/bin/music-ctl). Працюють звідусіль.
local music_ctl = os.getenv("HOME") .. "/.local/bin/music-ctl "
bind(mod .. " + SHIFT + F",      hl.dsp.exec_cmd(music_ctl .. "search"))   -- SUPER+Shift+F — пошук музики
bind(mod .. " + SHIFT + P",      hl.dsp.exec_cmd(music_ctl .. "play"))     -- SUPER+Shift+P — пауза / відтворення
bind(mod .. " + SHIFT + period", hl.dsp.exec_cmd(music_ctl .. "next"))     -- SUPER+Shift+. — наступний трек
bind(mod .. " + SHIFT + comma",  hl.dsp.exec_cmd(music_ctl .. "prev"))     -- SUPER+Shift+, — попередній трек
bind(mod .. " + SHIFT + D",      hl.dsp.exec_cmd(music_ctl .. "save"))     -- SUPER+Shift+D — скачати те, що грає, у ~/Music
bind(mod .. " + SHIFT + Q",      hl.dsp.exec_cmd(music_ctl .. "quit"))     -- SUPER+Shift+Q — вийти з музики (закрити панель)
bind(mod .. " + SHIFT + L",      hl.dsp.exec_cmd(music_ctl .. "repeat"))   -- SUPER+Shift+L — повтор одного треку (loop; Shift+R — перечитати конфіг)


-- ───────────── ЗНІМКИ ЕКРАНА (див. snip.py) ─────────────
-- Екран «заморожується», вгорі — панель режимів: 1 область · 2 вікно · 3 ласо · 4 екран (Tab — по колу),
-- E — редагувати після знімка, Esc — скасувати. Знімок → ~/Pictures/Screenshots + буфер обміну,
-- далі редактор satty: малювання, текст, стрілки, прямокутники, розмиття, маркер, обрізка.
local snip = os.getenv("HOME") .. "/.config/hypr/snip.py"
hl.bind("CTRL + ALT + Delete", hl.dsp.exec_cmd("~/.config/rofi/scripts/ctrlaltdel.sh"))  -- Ctrl+Alt+Del — меню: вимкнути / перезавантажити / BIOS / диспетчер / блокування (ще раз — закрити)
hl.bind("Print",          hl.dsp.exec_cmd(snip))                              -- Print — знімок або відео (V): область / вікно / ласо / екран; під час запису — зупинити
hl.bind("SHIFT + Print",  hl.dsp.exec_cmd(snip .. " --now --mode screen"))    -- Shift+Print — весь екран одразу в редактор
hl.bind("ALT + Print",    hl.dsp.exec_cmd(snip .. " --now --mode window"))    -- Alt+Print — активне вікно одразу в редактор
hl.bind("CTRL + Print",   hl.dsp.exec_cmd(snip .. " --no-edit"))              -- Ctrl+Print — область без редактора (зберегти + скопіювати)
bind(mod .. " + X",       hl.dsp.exec_cmd(snip))                              -- SUPER+X — те саме, що Print (якщо клавіші Print немає)
bind(mod .. " + SHIFT + X", hl.dsp.exec_cmd(snip .. " --now --mode screen --no-edit")) -- SUPER+Shift+X — швидкий знімок усього екрана (зберегти + скопіювати, без редактора)
bind(mod .. " + ALT + X",   hl.dsp.exec_cmd(snip .. " --video"))                 -- SUPER+Alt+X — те саме вікно, одразу на «Відео»; під час запису — зупинити


-- ───────────── ВІДЕО ПОВЕРХ ВІКОН (pip) ─────────────
-- Відкрити: кнопка «▶ Поверх вікон» на панелі закладок Firefox (як додати — ~/.config/theme/gen/pip-buttons.html) або SUPER+Alt+O.
-- Мишею (коли не замкнено): тягнути — перенести, коліщатко — розмір, подвійний клік — пауза.
local pip = os.getenv("HOME") .. "/.local/bin/pipctl "
bind(mod .. " + ALT + O",            hl.dsp.exec_cmd(pip .. "clipboard"))     -- SUPER+Alt+O — відкрити відео з буфера обміну поверх вікон
bind(mod .. " + ALT + V",            hl.dsp.exec_cmd(pip .. "toggle"))        -- SUPER+Alt+V — відео: показати / сховати
bind(mod .. " + ALT + L",            hl.dsp.exec_cmd(pip .. "lock"))          -- SUPER+Alt+L — відео: замок (кліки проходять крізь нього)
bind(mod .. " + ALT + bracketleft",  hl.dsp.exec_cmd(pip .. "opacity -0.05")) -- SUPER+Alt+[ — відео прозоріше (крок 5%, до 5%)
bind(mod .. " + ALT + bracketright", hl.dsp.exec_cmd(pip .. "opacity 0.05"))  -- SUPER+Alt+] — відео менш прозоре (крок 5%, до 100%)
bind(mod .. " + ALT + space",        hl.dsp.exec_cmd(pip .. "pause"))         -- SUPER+Alt+Пробіл — відео: пауза
bind(mod .. " + ALT + comma",        hl.dsp.exec_cmd(pip .. "seek -10"))      -- SUPER+Alt+, — відео: на 10 с назад
bind(mod .. " + ALT + period",       hl.dsp.exec_cmd(pip .. "seek 10"))       -- SUPER+Alt+. — відео: на 10 с вперед
bind(mod .. " + ALT + Q",            hl.dsp.exec_cmd(pip .. "quit"))          -- SUPER+Alt+Q — відео: закрити

-- ───────────── ІНСТРУМЕНТИ ─────────────
local rofi_scripts = os.getenv("HOME") .. "/.config/rofi/scripts/"
bind(mod .. " + slash",         hl.dsp.exec_cmd(rofi_scripts .. "keys.py"))        -- SUPER+/ — шпаргалка всіх клавіш (з пошуком)
bind(mod .. " + SHIFT + V",     hl.dsp.exec_cmd(rofi_scripts .. "clipboard.sh"))   -- SUPER+Shift+V — історія буфера обміну
bind(mod .. " + SHIFT + C",     hl.dsp.exec_cmd(rofi_scripts .. "colorpick.sh"))   -- SUPER+Shift+C — піпетка: колір з екрана в буфер
bind(mod .. " + period",        hl.dsp.exec_cmd(rofi_scripts .. "emoji.sh"))       -- SUPER+. — емодзі (вибране копіюється)
bind(mod .. " + SHIFT + W",     hl.dsp.exec_cmd(rofi_scripts .. "wallpaper.sh"))   -- SUPER+Shift+W — шпалери: вибрати з мініатюр
bind(mod .. " + CTRL + W",      hl.dsp.exec_cmd(rofi_scripts .. "wallpaper.sh next")) -- SUPER+Ctrl+W — наступні шпалери з колекції
local theme_cmd = os.getenv("HOME") .. "/.config/theme/theme"
bind(mod .. " + SHIFT + T",     hl.dsp.exec_cmd(theme_cmd))                         -- SUPER+Shift+T — тема: вибрати (світла, темна, свої)
bind(mod .. " + CTRL + T",      hl.dsp.exec_cmd(theme_cmd .. " toggle"))            -- SUPER+Ctrl+T — тема: темна ↔ світла


-- ───────────── МУЛЬТИМЕДІА (працюють і на заблокованому екрані) ─────────────
-- З плашкою на екрані (swayosd, ~/.config/swayosd/style.css); якщо swayosd не встановлено — як раніше, без плашки
local function osd(osd_args, fallback)
    return hl.dsp.exec_cmd("swayosd-client " .. osd_args .. " 2>/dev/null || " .. fallback)
end
hl.bind("XF86AudioRaiseVolume",  osd("--output-volume raise --max-volume 100", "wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+"), media) -- гучність +
hl.bind("XF86AudioLowerVolume",  osd("--output-volume lower",                  "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"),      media) -- гучність −
hl.bind("XF86AudioMute",         osd("--output-volume mute-toggle",            "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"),     media) -- вимкнути звук
hl.bind("XF86AudioMicMute",      osd("--input-volume mute-toggle",             "wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle"),   media) -- вимкнути мікрофон
hl.bind("XF86MonBrightnessUp",   osd("--brightness raise",                     "brightnessctl -e4 -n2 set 5%+"),                  media) -- яскравість +
hl.bind("XF86MonBrightnessDown", osd("--brightness lower",                     "brightnessctl -e4 -n2 set 5%-"),                  media) -- яскравість −

-- Через music-ctl media: керують тим, що зараз грає (і браузером теж), а якщо нічого
-- не грає — ВИБРАНИМ плеєром музичного центру (кнопки вгорі смуги музики), а не першим-ліпшим
hl.bind("XF86AudioNext",  hl.dsp.exec_cmd(music_ctl .. "media next"), { locked = true })  -- наступний трек
hl.bind("XF86AudioPrev",  hl.dsp.exec_cmd(music_ctl .. "media prev"), { locked = true })  -- попередній трек
hl.bind("XF86AudioPlay",  hl.dsp.exec_cmd(music_ctl .. "media play"), { locked = true })  -- пауза / відтворення
hl.bind("XF86AudioPause", hl.dsp.exec_cmd(music_ctl .. "media play"), { locked = true })  -- пауза / відтворення
