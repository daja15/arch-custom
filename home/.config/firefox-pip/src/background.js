// ╭──────────────────────────────────────────────────────────────╮
// │  ВІДЕО ПОВЕРХ ВІКОН — фон плагіна                            │
// │  Куди йде відео: native messaging → ~/.local/bin/pip-bridge  │
// │  → pipctl open (маленький плеєр поверх вікон).               │
// │  Звідки: кнопка на панелі, меню правого кліку, кнопки на      │
// │  самому відео (content.js).                                  │
// │  Сайти, що ховають відео (плеєр Kodik на сайтах аніме, інші  │
// │  вбудовані плеєри), yt-dlp не розбирає. Тому плагін пам'ятає │
// │  потік .m3u8 / .mpd, який вкладка завантажила сама, і віддає │
// │  плеєру його — з тим самим Referer і User-Agent, що й        │
// │  Firefox (без них такі сервери кажуть «заборонено»).         │
// ╰──────────────────────────────────────────────────────────────╯
const HOST = "pip_bridge";
const STREAM = /\.(m3u8|mpd)$/i;                     // плейлист потоку (за шляхом адреси)
const DIRECT = /\.(m3u8|mpd|mp4|webm|mkv|mov)$/i;    // пряме посилання на відео — не підміняємо
const KNOWN = /(^|\.)(youtube\.com|youtu\.be)$/i;    // yt-dlp знає краще (і браузер там не тягне m3u8)
const streams = new Map();                            // вкладка → { url, referer, time }

const pathOf = u => { try { return new URL(u).pathname; } catch (e) { return ""; } };
const hostOf = u => { try { return new URL(u).hostname; } catch (e) { return ""; } };

// Запам'ятати потік: перший плейлист — головний (далі хвилину-дві підтягуються його частини
// з якістю — їх пропускаємо); новий через 5+ с — уже інша серія / відео.
browser.webRequest.onBeforeRequest.addListener(d => {
  if (d.tabId < 0 || !STREAM.test(pathOf(d.url))) return;
  const old = streams.get(d.tabId);
  if (old && d.timeStamp - old.time < 5000) { old.time = d.timeStamp; return; }
  streams.set(d.tabId, { url: d.url, referer: d.documentUrl || d.originUrl || "", time: d.timeStamp });
}, { urls: ["<all_urls>"], types: ["xmlhttprequest", "media", "other"] });
// ───────────── ВІДЕО ЖИВЕ, ПОКИ ЖИВА ЙОГО ВКЛАДКА ─────────────
// Відео з вкладки тримає з мостом постійне з'єднання. Закрив вкладку або перейшов у ній на
// іншу сторінку — «close», і міст закриває плеєр; вийшов з Firefox — з'єднання рветься саме,
// і теж закриває. Перемкнувся на іншу вкладку — сторінка працює у фоні, відео грає далі.
// Нове відео (з будь-якої вкладки) — старі з'єднання «forget»: плеєр уже грає інше.
const watched = new Map();                            // вкладка → { port, page }
const pageOf = u => { try { const x = new URL(u); return x.origin + x.pathname + x.search; } catch (e) { return u; } };   // без #якоря

function release(tabId, how) {
  const w = watched.get(tabId);
  if (!w) return;
  watched.delete(tabId);
  try { w.port.postMessage({ action: how }); } catch (e) { /* міст уже вийшов */ }
  setTimeout(() => { try { w.port.disconnect(); } catch (e) {} }, 1000);
}

browser.tabs.onRemoved.addListener(id => { streams.delete(id); release(id, "close"); });
browser.tabs.onUpdated.addListener((id, ch) => {
  if (!ch.url) return;
  streams.delete(id);                                                  // інша сторінка
  const w = watched.get(id);
  if (w && pageOf(ch.url) !== w.page) release(id, "close");
});

async function send(action, url, tabId) {
  if (!url) return;
  const msg = { action, url };
  const s = streams.get(tabId);
  if (s && !DIRECT.test(pathOf(url)) && !KNOWN.test(hostOf(url))) {   // сторінка з схованим відео → її потік
    Object.assign(msg, { url: s.url, referer: s.referer, ua: navigator.userAgent, page: url });
  }
  for (const id of [...watched.keys()]) release(id, "forget");        // плеєр зараз замінить відео
  let tab = null;
  if (tabId >= 0) { try { tab = await browser.tabs.get(tabId); } catch (e) { /* вкладку вже закрили */ } }
  if (!tab) {                                                          // інше посилання — не прив'язуємо
    browser.runtime.sendNativeMessage(HOST, msg).catch(e =>
      console.error("pip: немає зв'язку з ~/.local/bin/pip-bridge —", e));
    return;
  }
  const port = browser.runtime.connectNative(HOST);
  watched.set(tabId, { port, page: pageOf(tab.url) });
  port.onDisconnect.addListener(p => {
    if (p.error) console.error("pip: немає зв'язку з ~/.local/bin/pip-bridge —", p.error);
    if (watched.get(tabId)?.port === port) watched.delete(tabId);      // плеєр закрили самі
  });
  port.postMessage(Object.assign(msg, { watch: true }));
}

// Кнопка на панелі: відео на сторінці (або сама сторінка) → поверх вікон
browser.browserAction.onClicked.addListener(async tab => {
  let url = tab.url;
  try {
    const r = await browser.tabs.sendMessage(tab.id, { q: "video" });   // найкраще посилання на відео в цій вкладці
    if (r && r.url) url = r.url;
  } catch (e) { /* сторінка без content.js (about:…) */ }
  send("open", url, tab.id);
});

// Меню правого кліку: по відео, посиланню чи сторінці
browser.contextMenus.create({ id: "pip", title: "▶ Відкрити поверх вікон", contexts: ["video", "audio", "link", "page", "frame"] });
browser.contextMenus.onClicked.addListener((info, tab) => {
  const src = info.srcUrl && /^https?:/.test(info.srcUrl) ? info.srcUrl : null;   // blob: (YouTube тощо) — не підходить
  const url = src || info.linkUrl || info.frameUrl || info.pageUrl || (tab && tab.url);
  send("open", url, info.linkUrl && !src ? -1 : tab && tab.id);   // інше посилання — не потік цієї вкладки
});

// Кнопки на самому відео (content.js)
browser.runtime.onMessage.addListener((m, sender) => { if (m && m.action) send(m.action, m.url, sender.tab && sender.tab.id); });
