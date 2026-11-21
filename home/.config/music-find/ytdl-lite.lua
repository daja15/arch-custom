-- ╭──────────────────────────────────────────────────────────────╮
-- │  YTDL-LITE — як mpv грає пісні з YouTube Music (music-find)  │
-- │                                                              │
-- │  Замість вбудованого в mpv модуля ytdl_hook: той для кожної  │
-- │  пісні бере від yt-dlp величезний JSON з усіма форматами й   │
-- │  НЕ звільняє пам'ять — mpv ріс на ~12 МБ з кожною піснею     │
-- │  (за 1,5 год музики — 250 МБ). Тут від yt-dlp беремо лише   │
-- │  три рядки: назву, виконавця й посилання на звук — пам'ять   │
-- │  стоїть на місці.                                            │
-- │                                                              │
-- │  Назва й виконавець передаються тегами (як у ytdl_hook) —    │
-- │  їх показують віджет музики, waybar, playerctl.              │
-- │  Підключає ~/.local/bin/music-find (mpv --ytdl=no --script=…)│
-- ╰──────────────────────────────────────────────────────────────╯
local FORMAT = "bestaudio[protocol^=m3u8]/bestaudio/best"   -- HLS: прямі посилання YouTube дають 403

local function esc(s)               -- рядок для EDL: %довжина%текст
    return "%" .. #s .. "%" .. s
end

mp.add_hook("on_load", 10, function()
    local url = mp.get_property("stream-open-filename", "")
    if not url:match("^https?://[^/]*youtu") then return end
    local r = mp.command_native({
        name = "subprocess", capture_stdout = true, playback_only = false,
        args = { "yt-dlp", "--no-warnings", "--no-playlist", "-f", FORMAT,
                 "-O", "%(title)s", "-O", "%(artist,uploader)s", "-O", "url", url },
    })
    local title, artist, stream = (r.stdout or ""):match("^(.-)\n(.-)\n(%S+)")
    if r.status ~= 0 or not stream then
        mp.msg.error("yt-dlp не дав посилання: " .. url)
        return
    end
    mp.set_property("file-local-options/force-media-title", title)
    mp.set_property("stream-open-filename",
        "edl://!global_tags,title=" .. esc(title) .. ",artist=" .. esc(artist) .. ";" .. esc(stream))
end)
