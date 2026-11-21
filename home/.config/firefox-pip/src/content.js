// ╭──────────────────────────────────────────────────────────────╮
// │  ВІДЕО ПОВЕРХ ВІКОН — кнопки на відео                        │
// │  Навів мишку на відео (будь-який сайт) — у лівому верхньому  │
// │  куті з'являється «▶ Поверх вікон». Відео на сторінці         │
// │  ставиться на паузу — далі воно грає в плеєрі.               │
// ╰──────────────────────────────────────────────────────────────╯
(() => {
  // Найкраще посилання: сам файл відео (якщо звичайне https), інакше — адреса сторінки / вбудованого плеєра
  const urlOf = v => (v && v.currentSrc && /^https?:/.test(v.currentSrc)) ? v.currentSrc : location.href;

  const bar = document.createElement("div");
  bar.className = "pipx-bar";
  for (const [action, text, title] of [["open", "▶ Поверх вікон", "Відкрити в маленькому плеєрі поверх усіх вікон"]]) {
    const b = document.createElement("button");
    b.className = "pipx-btn pipx-" + action;
    b.textContent = text;
    b.title = title;
    b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      const v = bar._video;
      if (v) v.pause();
      browser.runtime.sendMessage({ action, url: urlOf(v) });
      bar.classList.remove("pipx-show");
    }, true);
    bar.appendChild(b);
  }

  let hideTimer;
  function showOver(v) {
    const r = v.getBoundingClientRect();
    if (r.width < 200 || r.height < 120) return;                     // дрібні (прев'ю, реклама) — без кнопок
    if (!bar.isConnected) document.documentElement.appendChild(bar);
    bar._video = v;
    bar.style.left = (window.scrollX + r.left + 10) + "px";
    bar.style.top = (window.scrollY + r.top + 10) + "px";
    bar.classList.add("pipx-show");
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => bar.classList.remove("pipx-show"), 2500);
  }

  // Наведення: шукаємо відео під курсором (плеєри часто накривають його своїми шарами)
  let last = 0;
  document.addEventListener("mousemove", e => {
    const now = Date.now();
    if (now - last < 150) return;                                     // не частіше ~7 разів на секунду
    last = now;
    if (bar.contains(e.target)) { clearTimeout(hideTimer); return; }
    for (const v of document.querySelectorAll("video")) {
      const r = v.getBoundingClientRect();
      if (e.clientX >= r.left && e.clientX <= r.right && e.clientY >= r.top && e.clientY <= r.bottom) { showOver(v); return; }
    }
  }, { passive: true, capture: true });

  // Кнопка на панелі Firefox питає: яке тут відео?
  browser.runtime.onMessage.addListener(m => {
    if (!m || m.q !== "video") return;
    const vids = [...document.querySelectorAll("video")].sort((a, b) =>
      b.clientWidth * b.clientHeight - a.clientWidth * a.clientHeight);   // найбільше відео — головне
    const v = vids[0];
    if (v) v.pause();
    return Promise.resolve({ url: v ? urlOf(v) : (window === top ? location.href : null) });
  });
})();
