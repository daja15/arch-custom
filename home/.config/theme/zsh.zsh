# ╭──────────────────────────────────────────────────────────────╮
# │  ZSH ← ТЕМА: кольори промпту (powerlevel10k), підсвічування  │
# │  команд і підказок — з ~/.config/theme/gen/theme.sh (TH_…).  │
# │                                                              │
# │  Підключається в кінці ~/.zshrc. Змінив тему (theme / apply) │
# │  → у ВЖЕ ВІДКРИТИХ терміналах кольори оновляться самі перед  │
# │  наступним промптом (перевірка часу зміни theme.sh — дешево).│
# │                                                              │
# │  ~/.p10k.zsh не чіпаємо: його кольори-номери (31, 76, 178 …) │
# │  тут перекладаються на ролі теми (таблиця в _th_p10k).       │
# ╰──────────────────────────────────────────────────────────────╯
zmodload -F zsh/stat b:zstat 2>/dev/null
typeset -g _TH_FILE=$HOME/.config/theme/gen/theme.sh _TH_MTIME=
typeset -gA _TH_P10K_ORIG

_th_load() {                      # прочитати кольори теми
    [[ -r $_TH_FILE ]] || return 1
    source $_TH_FILE
    _TH_MTIME=$(zstat +mtime $_TH_FILE 2>/dev/null)
}

_th_styles() {                    # підказки за історією + підсвічування команд
    ZSH_AUTOSUGGEST_HIGHLIGHT_STYLE="fg=$TH_DIM"
    (( ${+ZSH_HIGHLIGHT_STYLES} )) || return 0
    ZSH_HIGHLIGHT_STYLES[command]="fg=$TH_ACCENT"                 # відома команда
    ZSH_HIGHLIGHT_STYLES[builtin]="fg=$TH_ACCENT"
    ZSH_HIGHLIGHT_STYLES[alias]="fg=$TH_ACCENT"
    ZSH_HIGHLIGHT_STYLES[function]="fg=$TH_BLUE"
    ZSH_HIGHLIGHT_STYLES[precommand]="fg=$TH_ACCENT,italic"       # sudo, exec …
    ZSH_HIGHLIGHT_STYLES[reserved-word]="fg=$TH_ACCENT2"          # if, for, do …
    ZSH_HIGHLIGHT_STYLES[unknown-token]="fg=$TH_RED"              # такої команди немає
    ZSH_HIGHLIGHT_STYLES[single-quoted-argument]="fg=$TH_GREEN"   # рядки
    ZSH_HIGHLIGHT_STYLES[double-quoted-argument]="fg=$TH_GREEN"
    ZSH_HIGHLIGHT_STYLES[dollar-quoted-argument]="fg=$TH_GREEN"
    ZSH_HIGHLIGHT_STYLES[path]="fg=$TH_TEXT,underline"            # наявний шлях
    ZSH_HIGHLIGHT_STYLES[globbing]="fg=$TH_YELLOW"                # * ? […]
    ZSH_HIGHLIGHT_STYLES[single-hyphen-option]="fg=$TH_YELLOW"    # -v
    ZSH_HIGHLIGHT_STYLES[double-hyphen-option]="fg=$TH_YELLOW"    # --help
    ZSH_HIGHLIGHT_STYLES[commandseparator]="fg=$TH_ACCENT2"       # ; && |
    ZSH_HIGHLIGHT_STYLES[redirection]="fg=$TH_ACCENT2"            # > <
    ZSH_HIGHLIGHT_STYLES[comment]="fg=$TH_DIM,italic"
}

_th_p10k() {                      # кольори powerlevel10k → ролі теми
    emulate -L zsh -o extendedglob
    local -A map
    local n
    for n in 31 37 38 39;                      do map[$n]=$TH_ACCENT;  done   # тека, акцентні сегменти
    for n in 32 33 67 68 74 81 110 117;        do map[$n]=$TH_BLUE;    done
    for n in 28 34 35 70 72 76 106;            do map[$n]=$TH_GREEN;   done   # git чистий, статус OK
    for n in 94 130 166 172 178 208 215 220;   do map[$n]=$TH_YELLOW;  done   # git змінено, попередження
    for n in 125 160 161 168 196;              do map[$n]=$TH_RED;     done   # помилка
    for n in 96 99 103 129 134 135;            do map[$n]=$TH_ACCENT2; done
    for n in 66 180 248;                       do map[$n]=$TH_MUTED;   done   # час, тривалість, user@host
    map[244]=$TH_FAINT                                                       # рамка багаторядкового промпту
    map[250]=$TH_DIM                                                         # роздільники, «on», «took»
    map[255]=$TH_TEXT                                                        # іконка ОС
    map[240]=$TH_SURFACE                                                     # фон сегментів

    local p v
    if (( ! ${#_TH_P10K_ORIG} )); then        # запам'ятати початкові значення з ~/.p10k.zsh (раз)
        for p in ${(k)parameters[(I)POWERLEVEL9K_*]}; do
            [[ ${parameters[$p]} == scalar* ]] || continue
            v=${(P)p}
            [[ $v == <-> || $v == *%<->F* ]] && _TH_P10K_ORIG[$p]=$v
        done
    fi
    for p v in ${(kv)_TH_P10K_ORIG}; do
        if [[ $v == <-> ]]; then
            typeset -g $p=${map[$v]:-$v}
        else
            typeset -g $p=${v//(#b)%(<->)F/%F\{${map[$match[1]]:-$match[1]}\}}
        fi
    done
}

_th_check() {                     # перед кожним промптом: чи не змінилась тема?
    [[ -r $_TH_FILE ]] || return 0
    [[ $(zstat +mtime $_TH_FILE 2>/dev/null) == $_TH_MTIME ]] && return 0
    _th_load && _th_styles && _th_p10k
    (( ${+functions[p10k]} )) && p10k reload
}

_th_load && _th_styles && _th_p10k
precmd_functions=(_th_check ${precmd_functions:#_th_check})   # першим — до того, як p10k збере промпт
