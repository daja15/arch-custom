// ╭──────────────────────────────────────────────────────────────╮
// │  Firefox: підключити mozilla.cfg (дозвіл непідписаних        │
// │  плагінів — плагін «Відео поверх вікон»).                    │
// │  Ставиться в /usr/lib/firefox/defaults/pref/ (потрібен sudo, │
// │  див. ~/.config/firefox-pip/README.md).                      │
// ╰──────────────────────────────────────────────────────────────╯
pref("general.config.filename", "mozilla.cfg");
pref("general.config.obscure_value", 0);
pref("general.config.sandbox_enabled", false);
