// ╭──────────────────────────────────────────────────────────────╮
// │  FIREFOX — менше пам'яті й фонової роботи                    │
// │  Firefox читає цей файл при кожному запуску й перекриває ним │
// │  налаштування з about:config. Відкотити: видалити рядок      │
// │  (або весь файл) і скинути параметр в about:config.          │
// ╰──────────────────────────────────────────────────────────────╯

// Стартова сторінка — стандартна Firefox (свою з 127.0.0.1:47800 прибрано 2026-11-18).

// ───────────── НОВА ВКЛАДКА (Ctrl+T) ─────────────
// Замість важкої сторінки з плитками, погодою й новинами — порожня, і без
// заздалегідь завантаженої копії у фоні (це окремий процес ~40 МБ).
user_pref("browser.newtabpage.enabled", false);
user_pref("browser.newtab.preload", false);
user_pref("browser.newtabpage.activity-stream.feeds.topsites", false);
user_pref("browser.newtabpage.activity-stream.feeds.section.topstories", false);
user_pref("browser.newtabpage.activity-stream.feeds.weatherfeed", false);
user_pref("browser.newtabpage.activity-stream.showSponsored", false);
user_pref("browser.newtabpage.activity-stream.showSponsoredTopSites", false);
user_pref("browser.newtabpage.activity-stream.feeds.telemetry", false);
user_pref("browser.newtabpage.activity-stream.telemetry", false);

// ───────────── ПРОЦЕСИ ─────────────
// Скільки порожніх процесів тримати напоготові для нових вкладок (було 3 по ~20 МБ).
user_pref("dom.ipc.processPrelaunch.fission.number", 1);

// ───────────── ПАМ'ЯТЬ ─────────────
// Скільки попередніх сторінок тримати в пам'яті для миттєвої кнопки «Назад»
// (за замовчуванням з 40 ГБ RAM — 8). Далі «Назад» просто перезавантажить сторінку.
user_pref("browser.sessionhistory.max_total_viewers", 2);
// Коли пам'яті мало — вивантажувати вкладки, якими давно не користувався
// (вкладка лишається у списку й завантажиться знову при відкритті).
user_pref("browser.tabs.unloadOnLowMemory", true);

// ───────────── ФОНОВА РОБОТА ─────────────
// Телеметрія й звіти: зайві процеси, таймери й мережеві запити.
user_pref("datareporting.healthreport.uploadEnabled", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.enabled", false);
user_pref("toolkit.telemetry.unified", false);
user_pref("toolkit.telemetry.archive.enabled", false);
user_pref("app.shield.optoutstudies.enabled", false);      // «дослідження» Mozilla
user_pref("app.normandy.enabled", false);                  // віддалені експерименти з налаштуваннями
user_pref("browser.discovery.enabled", false);             // рекомендації розширень
user_pref("extensions.pocket.enabled", false);
// Зберігати сесію раз на 60 с замість 15 (менше записів на диск і роботи у фоні)
user_pref("browser.sessionstore.interval", 60000);
