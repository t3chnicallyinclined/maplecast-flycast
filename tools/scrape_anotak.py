#!/usr/bin/env python3
"""
Crawl anotak's MVC2 DAT dumps (https://zachd.com/mvc2/data/anotak/) into a local
structured reference under refs/anotak/. Faithful table parse (no LLM), raw-HTML
cached so re-runs are cheap/resumable. Polite throttle.

Usage:
  python tools/scrape_anotak.py attacks      # 59 PLxx_DAT_atk.html  (fast)
  python tools/scrape_anotak.py animations   # all animgroup pages   (heavy, ~1600)
  python tools/scrape_anotak.py all          # both (default)

Output:
  refs/anotak/_raw/<page>.html          cached source
  refs/anotak/attacks/PLxx.json         parsed attack table (incl. Hitspark, move, anim link)
  refs/anotak/animations/PLxx.json      parsed animation cells per group
  refs/anotak/manifest.json             chars, counts, field lists, source URLs, fetch date
"""
import sys, os, re, json, time, urllib.request
from html.parser import HTMLParser
from html import unescape

BASE = "https://zachd.com/mvc2/data/anotak/"
ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "refs", "anotak")
RAW = os.path.join(ROOT, "_raw")
UA = "Mozilla/5.0 (MapleCast anotak reference crawler; one-time)"
THROTTLE = 0.15  # seconds between live fetches (be polite to zachd.com)


def fetch(page):
    """Return page HTML, caching raw under refs/anotak/_raw/."""
    os.makedirs(RAW, exist_ok=True)
    cache = os.path.join(RAW, page)
    if os.path.exists(cache):
        with open(cache, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    req = urllib.request.Request(BASE + page, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        html = r.read().decode("utf-8", errors="replace")
    with open(cache, "w", encoding="utf-8") as f:
        f.write(html)
    time.sleep(THROTTLE)
    return html


class TableParser(HTMLParser):
    """Extract every <table> as {headers:[...], rows:[[cell,...]], links:[[href|None,...]]}.
    Each cell is its stripped text; per-cell first <a href> is captured in links."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.tables = []
        self._t = None          # current table
        self._row = None        # current row cells
        self._rowlinks = None
        self._cell = None       # current cell text parts
        self._celllink = None
        self._in_head = False

    def handle_starttag(self, tag, attrs):
        if tag == "table":
            self._t = {"headers": [], "rows": [], "links": []}
        elif tag == "thead":
            self._in_head = True
        elif tag == "tr" and self._t is not None:
            self._row = []
            self._rowlinks = []
        elif tag in ("td", "th") and self._row is not None:
            self._cell = []
            self._celllink = None
        elif tag == "a" and self._cell is not None and self._celllink is None:
            for k, v in attrs:
                if k == "href":
                    self._celllink = v

    def handle_data(self, data):
        if self._cell is not None:
            self._cell.append(data)

    def handle_entityref(self, name):
        if self._cell is not None:
            self._cell.append(unescape("&%s;" % name))

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self._cell is not None:
            text = re.sub(r"\s+", " ", "".join(self._cell)).strip()
            if tag == "th" and self._in_head:
                self._t["headers"].append(text)
            else:
                self._row.append(text)
                self._rowlinks.append(self._celllink)
            self._cell = None
            self._celllink = None
        elif tag == "thead":
            self._in_head = False
        elif tag == "tr" and self._row is not None:
            # header rows (th) populate headers; data rows populate rows
            if self._row and not self._in_head:
                # a tr made entirely of th in <thead> leaves self._row empty of data
                if any(self._row):
                    self._t["rows"].append(self._row)
                    self._t["links"].append(self._rowlinks)
            self._row = None
            self._rowlinks = None
        elif tag == "table" and self._t is not None:
            self.tables.append(self._t)
            self._t = None


def parse_tables(html):
    p = TableParser()
    p.feed(html)
    return p.tables


def page_title(html):
    m = re.search(r"<title>(.*?)</title>", html, re.I | re.S)
    return unescape(re.sub(r"\s+", " ", m.group(1)).strip()) if m else ""


def char_list():
    html = fetch("index.html")
    chars = sorted(set(re.findall(r"PL([0-9A-Fa-f]{2})_DAT_atk\.html", html)))
    return [c.upper() for c in chars]


def crawl_attacks(chars):
    outdir = os.path.join(ROOT, "attacks")
    os.makedirs(outdir, exist_ok=True)
    summary = {}
    for c in chars:
        page = f"PL{c}_DAT_atk.html"
        html = fetch(page)
        tables = parse_tables(html)
        if not tables:
            print(f"  PL{c}: NO TABLE"); continue
        t = tables[0]
        headers = t["headers"]
        attacks = []
        anim_groups = set()
        for row, links in zip(t["rows"], t["links"]):
            rec = dict(zip(headers, row))
            # first cell: "<num> <MOVE>" with an anim link
            first = row[0] if row else ""
            alink = links[0] if links else None
            mm = re.match(r"(\d+)\s*(.*)", first)
            if mm:
                rec["attack number"] = mm.group(1)
                if mm.group(2):
                    rec["move"] = mm.group(2)
            if alink:
                rec["anim_link"] = alink
                g = re.search(r"animgroup(\d+)", alink)
                if g:
                    anim_groups.add(int(g.group(1)))
            attacks.append(rec)
        out = {
            "char": f"PL{c}",
            "title": page_title(html),
            "source": BASE + page,
            "fields": headers,
            "attack_count": len(attacks),
            "anim_groups_referenced": sorted(anim_groups),
            "attacks": attacks,
        }
        with open(os.path.join(outdir, f"PL{c}.json"), "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
        summary[f"PL{c}"] = {"attacks": len(attacks), "title": out["title"],
                             "anim_groups": sorted(anim_groups)}
        print(f"  PL{c}: {len(attacks)} attacks, {len(anim_groups)} anim groups  [{out['title']}]")
    return summary


def crawl_animations(chars, groups_by_char):
    outdir = os.path.join(ROOT, "animations")
    os.makedirs(outdir, exist_ok=True)
    total_pages = 0
    for c in chars:
        # Discover ALL animgroups for this char from its (cached) attack page —
        # the attack table only links the ~6 attack groups, but the page header
        # links every animgroup the character has (walk, jump, specials, etc.).
        groups = []
        atk_cache = os.path.join(RAW, f"PL{c}_DAT_atk.html")
        if os.path.exists(atk_cache):
            with open(atk_cache, "r", encoding="utf-8", errors="replace") as f:
                groups = sorted({int(g) for g in re.findall(r"animgroup(\d+)", f.read())})
        if not groups:
            groups = list(range(0, 28))
        char_anim = {"char": f"PL{c}", "source_base": BASE, "groups": {}}
        for g in groups:
            page = f"PL{c}_DAT_animgroup{g}.html"
            try:
                html = fetch(page)
            except Exception as e:
                continue
            tables = parse_tables(html)
            cells = []
            hdrs = []
            for t in tables:
                if t["headers"]:
                    hdrs = t["headers"]
                for row in t["rows"]:
                    cells.append(dict(zip(hdrs, row)) if hdrs else row)
            char_anim["groups"][str(g)] = {"fields": hdrs, "cell_count": len(cells), "cells": cells}
            total_pages += 1
        with open(os.path.join(outdir, f"PL{c}.json"), "w", encoding="utf-8") as f:
            json.dump(char_anim, f, indent=1)
        print(f"  PL{c}: {len(char_anim['groups'])} groups, "
              f"{sum(v['cell_count'] for v in char_anim['groups'].values())} cells")
    return total_pages


def crawl_fields():
    """Crawl the field-dictionary layer: possible.html (28 attack fields) and
    possible_anim.html (17 anim fields), then each possible_<Field>.html value
    table (Value/Hex/Characters/Entries + per-value char/move detail)."""
    out = {"attack": [], "anim": []}
    for index_page, sub, pat, strip in (
        ("possible.html", "attack", r"(possible_[A-Za-z0-9]+\.html)", r"^possible_"),
        ("possible_anim.html", "anim", r"(possible_anim_[A-Za-z0-9]+\.html)", r"^possible_anim_"),
    ):
        html = fetch(index_page)
        links = sorted(set(re.findall(pat, html)))
        if sub == "attack":
            links = [l for l in links if not l.startswith("possible_anim")]
        outdir = os.path.join(ROOT, "fields", sub)
        os.makedirs(outdir, exist_ok=True)
        for page in links:
            fhtml = fetch(page)
            tables = parse_tables(fhtml)
            field = re.sub(strip, "", page).replace(".html", "")
            rec = {"field": field, "source": BASE + page, "title": page_title(fhtml),
                   "tables": [{"headers": t["headers"], "rows": t["rows"]} for t in tables]}
            with open(os.path.join(outdir, field + ".json"), "w", encoding="utf-8") as f:
                json.dump(rec, f, indent=1)
            out[sub].append(field)
            print(f"  {sub}/{field}: {sum(len(t['rows']) for t in tables)} rows")
    return out


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    os.makedirs(ROOT, exist_ok=True)
    chars = char_list()
    print(f"{len(chars)} characters: {' '.join(chars)}")
    manifest = {"source": BASE, "characters": [f"PL{c}" for c in chars],
                "attack_field_index": BASE + "possible.html",
                "anim_field_index": BASE + "possible_anim.html"}
    summary = {}
    if mode in ("fields", "all"):
        print("=== FIELD DICTIONARIES ===")
        manifest["fields"] = crawl_fields()
    if mode in ("attacks", "all"):
        print("=== ATTACKS ===")
        summary = crawl_attacks(chars)
        manifest["attacks"] = summary
    if mode in ("animations", "all"):
        print("=== ANIMATIONS ===")
        n = crawl_animations(chars, summary)
        manifest["animation_pages"] = n
    with open(os.path.join(ROOT, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    print(f"manifest -> {os.path.join(ROOT, 'manifest.json')}")


if __name__ == "__main__":
    main()
