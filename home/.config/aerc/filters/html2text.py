#!/usr/bin/env python3
# Фільтр aerc: HTML-лист → читабельний текст (запасний варіант, якщо немає w3m).
# Лише стандартна бібліотека Python. Читає HTML зі stdin, пише текст у stdout.
#   • заголовки й жирне — жирним, посилання — «текст [адреса]»
#   • списки — з «•», таблиці — рядками з « │ » між клітинками
#   • скрипти, стилі й приховані блоки пропускаються
import html
import re
import shutil
import sys
import textwrap
from html.parser import HTMLParser

WIDTH = min(shutil.get_terminal_size((100, 40)).columns, 100)
BOLD, DIM, ACC, R = "\033[1m", "\033[2m", "\033[38;2;136;192;208m", "\033[0m"

BLOCK = {"p", "div", "section", "article", "header", "footer", "table", "tr", "ul", "ol",
         "blockquote", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "center", "form", "hr"}
SKIP = {"script", "style", "head", "title", "noscript", "svg", "template"}


class ToText(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.out = []          # абзаци
        self.cur = []          # поточний абзац
        self.skip = 0
        self.href = None
        self.link_text = []
        self.pre = 0
        self.cell = False

    def flush(self):
        text = "".join(self.cur)
        if not self.pre:
            text = re.sub(r"[ \t\r\n ]+", " ", text).strip()
        if text:
            self.out.append(text)
        self.cur = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        style = (a.get("style") or "").replace(" ", "").lower()
        if tag in SKIP or "display:none" in style:
            self.skip += 1
            return
        if self.skip:
            return
        if tag in BLOCK:
            self.flush()
        if tag == "br":
            self.flush()
        elif tag in ("h1", "h2", "h3", "h4", "h5", "h6", "b", "strong"):
            self.cur.append(BOLD)
        elif tag == "li":
            self.flush()
            self.cur.append("  • ")
        elif tag in ("td", "th"):
            if self.cell:
                self.cur.append(" │ ")
            self.cell = True
        elif tag == "tr":
            self.cell = False
        elif tag == "a":
            self.href = a.get("href")
            self.link_text = []
        elif tag == "img":
            alt = (a.get("alt") or "").strip()
            if alt:
                self.cur.append(f"{DIM}[{alt}]{R}")
        elif tag == "pre":
            self.pre += 1
        elif tag == "hr":
            self.out.append(DIM + "─" * min(WIDTH, 60) + R)

    def handle_endtag(self, tag):
        if tag in SKIP:
            self.skip = max(0, self.skip - 1)
            return
        if self.skip:
            return
        if tag in ("h1", "h2", "h3", "h4", "h5", "h6", "b", "strong"):
            self.cur.append(R)
        if tag == "a" and self.href:
            text = "".join(self.link_text).strip()
            href = self.href
            if href.startswith(("http", "mailto:")) and href.rstrip("/") != text.rstrip("/"):
                self.cur.append(f" {ACC}[{href}]{R}")
            self.href = None
        if tag == "pre":
            self.pre = max(0, self.pre - 1)
        if tag in BLOCK or tag in ("li",):
            self.flush()

    def handle_data(self, data):
        if self.skip:
            return
        self.cur.append(data)
        if self.href is not None:
            self.link_text.append(data)


def main():
    raw = sys.stdin.buffer.read().decode("utf-8", errors="replace")
    p = ToText()
    p.feed(raw)
    p.close()
    p.flush()
    blank = False
    for para in p.out:
        if not para.strip():
            continue
        lines = para.split("\n") if "\n" in para else textwrap.wrap(html.unescape(para), WIDTH) or [""]
        for line in lines:
            print(line)
        print()


if __name__ == "__main__":
    main()
