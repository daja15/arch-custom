-- ╭──────────────────────────────────────────────────────────────╮
-- │  ВИГЛЯД                                                      │
-- │  Головний перемикач — ECO нижче.                             │
-- │    ECO = true  → без блюру, тіней, затемнення; мінімум       │
-- │                  анімацій (рекомендовано, GPU майже спить)   │
-- │    ECO = false → тонка тінь + повні анімації (трохи гарніше, │
-- │                  трохи більше роботи для GPU)                │
-- ╰──────────────────────────────────────────────────────────────╯

local ECO = true   -- ← ГОЛОВНИЙ ПЕРЕМИКАЧ ЕНЕРГОЗБЕРЕЖЕННЯ


hl.config({
    general = {
        gaps_in     = THEME.gaps_in,    -- відступ між вікнами (theme.conf)
        gaps_out    = { top = THEME.gaps_top, right = THEME.gaps_out,   -- відступ від вікон до країв екрана (theme.conf):
                        bottom = THEME.gaps_out, left = THEME.gaps_out }, -- зверху більший
        border_size = THEME.border,     -- товщина рамки вікна (theme.conf)

        col = {
            active_border   = C.accent,   -- рамка активного вікна (суцільна — градієнт дорожчий)
            inactive_border = C.line,     -- рамка неактивних вікон
        },

        resize_on_border = true,   -- змінювати розмір, тягнучи за рамку
        extend_border_grab_area = 10, -- зона захоплення рамки (px)
        hover_icon_on_border = true,  -- іконка-стрілка над рамкою
        allow_tearing    = false,  -- tearing (для ігор), див. вікі перед увімкненням
        layout           = "dwindle", -- лейаут: dwindle / master / scrolling
    },

    decoration = {
        rounding       = THEME.radius,  -- заокруглення кутів (theme.conf; 0 = гострі, найдешевше)
        rounding_power = 2.0,   -- форма заокруглення: 2.0 = коло, 4.0 = «squircle»

        active_opacity   = 1.0, -- прозорість активного вікна (1.0 = непрозоре; <1 змушує GPU змішувати шари)
        inactive_opacity = 1.0, -- прозорість неактивних вікон

        dim_inactive = false,   -- затемнювати неактивні вікна (додатковий прохід рендеру)
        dim_strength = 0.15,    -- сила затемнення, якщо увімкнено
        dim_special  = 0,       -- затемнення фону під спец-робочим столом (0 = вимк: кожен кадр анімацій під терміналом коштував Hyprland ~5% процесора й відеочипа)

        blur = {
            enabled = false,          -- БЛЮР — найдорожчий ефект, вимкнено
            size    = 3,              -- радіус (якщо увімкнеш)
            passes  = 1,              -- кількість проходів (кожен — ще навантаження)
            new_optimizations = true, -- оптимізації блюру
            xray    = true,           -- блюрити лише шпалери, а не вікна під вікном (дешевше)
            popups  = false,          -- блюр спливних меню
            special = false,          -- блюр під scratchpad
        },

        shadow = {
            enabled      = not ECO,              -- тінь вікон (вимк в ECO)
            range        = 12,                   -- розмір тіні (px)
            render_power = 3,                    -- різкість спаду тіні (1–4)
            color        = "rgba(0a0b0e99)",     -- колір тіні активного вікна
            color_inactive = "rgba(0a0b0e55)",   -- колір тіні неактивних
        },
    },

    animations = {
        enabled = true,                -- анімації (тривають частки секунди; у стані спокою GPU не навантажують)
        workspace_wraparound = false,  -- з останнього столу не стрибати на перший
    },
})


-- ───────────── АНІМАЦІЇ ─────────────
-- speed — тривалість у децисекундах (1 = 100 мс). Менше = коротше = менше кадрів.
-- https://wiki.hypr.land/configuring/core/animations/

hl.curve("snappy",  { type = "bezier", points = { {0.22, 1},  {0.36, 1} } })   -- швидкий старт, м'який фініш
hl.curve("linear",  { type = "bezier", points = { {0, 0},     {1, 1}    } })

if ECO then
    -- Мінімальний набір: тільки короткі фейди й ковзання столів
    hl.animation({ leaf = "global",     enabled = true,  speed = 3,   bezier = "snappy" })
    hl.animation({ leaf = "windowsIn",  enabled = true,  speed = 2.5, bezier = "snappy", style = "popin 90%" }) -- поява вікна
    hl.animation({ leaf = "windowsOut", enabled = true,  speed = 1.5, bezier = "linear", style = "popin 90%" }) -- закриття вікна
    hl.animation({ leaf = "windowsMove",enabled = true,  speed = 2.5, bezier = "snappy" })                      -- переміщення вікна
    hl.animation({ leaf = "border",     enabled = false })                                                       -- анімація кольору рамки — вимк
    hl.animation({ leaf = "fade",       enabled = true,  speed = 2,   bezier = "linear" })                      -- фейди
    hl.animation({ leaf = "layers",     enabled = true,  speed = 2,   bezier = "snappy", style = "fade" })      -- waybar / rofi
    hl.animation({ leaf = "workspaces", enabled = true,  speed = 2.5, bezier = "snappy", style = "slide" })     -- перемикання столів
    hl.animation({ leaf = "specialWorkspace", enabled = true, speed = 2.5, bezier = "snappy", style = "slidevert" }) -- scratchpad
else
    -- Повний набір: трохи довші й плавніші
    hl.curve("spring", { type = "spring", mass = 1, stiffness = 240, damping = 24 })
    hl.animation({ leaf = "global",     enabled = true,  speed = 5,   bezier = "snappy" })
    hl.animation({ leaf = "windows",    enabled = true,  speed = 4.5, spring = "spring" })
    hl.animation({ leaf = "windowsIn",  enabled = true,  speed = 4,   spring = "spring", style = "popin 85%" })
    hl.animation({ leaf = "windowsOut", enabled = true,  speed = 2,   bezier = "linear", style = "popin 85%" })
    hl.animation({ leaf = "border",     enabled = true,  speed = 4,   bezier = "snappy" })
    hl.animation({ leaf = "fade",       enabled = true,  speed = 3,   bezier = "snappy" })
    hl.animation({ leaf = "layers",     enabled = true,  speed = 3,   bezier = "snappy", style = "slide" })
    hl.animation({ leaf = "workspaces", enabled = true,  speed = 4,   bezier = "snappy", style = "slide" })
    hl.animation({ leaf = "specialWorkspace", enabled = true, speed = 4, bezier = "snappy", style = "slidevert" })
end


-- ───────────── ГРУПИ ВІКОН (вкладки) ─────────────
hl.config({
    group = {
        col = {
            border_active   = C.accent2,  -- рамка активної групи
            border_inactive = C.line,     -- рамка неактивної групи
        },
        -- Вкладки як у браузері: суцільні «ярлички» впритул до вікна; активна — кольору рамки групи,
        -- тож виглядає частиною вікна, решта — темніші
        groupbar = {
            enabled          = true,
            font_family      = THEME.font_ui,
            font_size        = 11,
            height           = 24,             -- висота вкладки
            gradients        = true,           -- суцільна заливка вкладки (а не лише смужка)
            gradient_rounding = THEME.radius,  -- заокруглення — як у кутів вікна (theme.conf)
            gradient_round_only_edges = true,  -- заокруглені лише зовнішні кути: вкладки — одна смуга, як у вікна
            indicator_height = 0,              -- без окремої тонкої смужки
            gaps_in          = 2,              -- тонкий проміжок між вкладками
            gaps_out         = 6,              -- відступ між вкладками й вікном (px)
            keep_upper_gap   = false,
            text_color       = C.bg,           -- текст активної вкладки (на лавандовому)
            text_color_inactive = C.text,      -- назви неактивних вкладок читаються
            font_weight_active   = "bold",
            col = {
                active   = C.accent2,          -- = рамка активної групи
                inactive = C.line,             -- світліше за фон: неактивні вкладки видно
            },
        },
    },
})
