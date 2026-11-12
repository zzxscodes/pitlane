#!/usr/bin/env python3
"""Build a static API site from API.md / API_CN.md (stdlib only)."""

from __future__ import annotations

import html
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "site"

CSS = """
:root {
  --bg: #0f1419;
  --panel: #1a2332;
  --text: #e7ecf3;
  --muted: #9aa7b8;
  --accent: #3d8bfd;
  --border: #2a3545;
  --code: #121820;
}
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; font-family: "IBM Plex Sans", "Segoe UI", sans-serif; background: var(--bg); color: var(--text); }
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
.layout { display: grid; grid-template-columns: 280px 1fr; height: 100%; }
.side { background: var(--panel); border-right: 1px solid var(--border); overflow: auto; padding: 1rem 0.75rem 2rem; }
.side h1 { font-size: 1.05rem; margin: 0.25rem 0.5rem 0.75rem; }
.side .lang { display: flex; gap: 0.5rem; margin: 0 0.5rem 1rem; }
.side .lang a { font-size: 0.85rem; padding: 0.2rem 0.5rem; border: 1px solid var(--border); border-radius: 4px; color: var(--muted); }
.side .lang a.active { color: var(--text); border-color: var(--accent); }
.side nav a { display: block; padding: 0.35rem 0.5rem; color: var(--muted); border-radius: 4px; font-size: 0.9rem; }
.side nav a:hover, .side nav a.active { background: #243044; color: var(--text); text-decoration: none; }
.main { overflow: auto; padding: 1.5rem 2rem 3rem; max-width: 920px; }
.main h1 { font-size: 1.75rem; margin-top: 0; }
.main h2 { font-size: 1.25rem; margin-top: 2rem; border-bottom: 1px solid var(--border); padding-bottom: 0.35rem; }
.main h3 { font-size: 1.05rem; margin-top: 1.4rem; }
.main p, .main li { line-height: 1.65; color: var(--text); }
.main code { font-family: "IBM Plex Mono", ui-monospace, monospace; background: var(--code); padding: 0.1rem 0.35rem; border-radius: 3px; font-size: 0.9em; }
.main pre { background: var(--code); border: 1px solid var(--border); border-radius: 6px; padding: 0.9rem 1rem; overflow: auto; }
.main pre code { padding: 0; background: transparent; }
.main table { border-collapse: collapse; width: 100%; margin: 1rem 0; font-size: 0.92rem; }
.main th, .main td { border: 1px solid var(--border); padding: 0.45rem 0.6rem; text-align: left; vertical-align: top; }
.main th { background: var(--panel); }
.meta { color: var(--muted); font-size: 0.9rem; margin-bottom: 1.5rem; }
@media (max-width: 860px) {
  .layout { grid-template-columns: 1fr; }
  .side { border-right: 0; border-bottom: 1px solid var(--border); max-height: 40vh; }
}
"""


def slugify(title: str) -> str:
    s = title.strip().lower()
    s = re.sub(r"[^a-z0-9\u4e00-\u9fff]+", "-", s)
    return s.strip("-") or "section"


def inline_md(text: str) -> str:
    text = html.escape(text)
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', text)
    return text


def render_body(md: str) -> str:
    lines = md.splitlines()
    out: list[str] = []
    i = 0
    in_code = False
    code_lang = ""
    code_buf: list[str] = []
    in_ul = False
    in_table = False

    def close_ul():
        nonlocal in_ul
        if in_ul:
            out.append("</ul>")
            in_ul = False

    def close_table():
        nonlocal in_table
        if in_table:
            out.append("</tbody></table>")
            in_table = False

    while i < len(lines):
        line = lines[i]
        if line.startswith("```"):
            close_ul()
            close_table()
            if not in_code:
                in_code = True
                code_lang = line[3:].strip()
                code_buf = []
            else:
                out.append(f'<pre><code class="language-{html.escape(code_lang)}">')
                out.append(html.escape("\n".join(code_buf)))
                out.append("</code></pre>")
                in_code = False
            i += 1
            continue
        if in_code:
            code_buf.append(line)
            i += 1
            continue
        if not line.strip():
            close_ul()
            close_table()
            i += 1
            continue
        if line.startswith("|") and "|" in line[1:]:
            close_ul()
            cells = [c.strip() for c in line.strip("|").split("|")]
            if all(re.fullmatch(r":?-+:?", c.replace(" ", "")) for c in cells):
                i += 1
                continue
            if not in_table:
                out.append("<table><thead><tr>")
                for c in cells:
                    out.append(f"<th>{inline_md(c)}</th>")
                out.append("</tr></thead><tbody>")
                in_table = True
            else:
                out.append("<tr>")
                for c in cells:
                    out.append(f"<td>{inline_md(c)}</td>")
                out.append("</tr>")
            i += 1
            continue
        close_table()
        if line.startswith("### "):
            close_ul()
            t = line[4:].strip()
            out.append(f'<h3 id="{slugify(t)}">{inline_md(t)}</h3>')
        elif line.startswith("## "):
            close_ul()
            t = line[3:].strip()
            out.append(f'<h2 id="{slugify(t)}">{inline_md(t)}</h2>')
        elif line.startswith("# "):
            close_ul()
            t = line[2:].strip()
            out.append(f"<h1>{inline_md(t)}</h1>")
        elif line.startswith("- "):
            if not in_ul:
                out.append("<ul>")
                in_ul = True
            out.append(f"<li>{inline_md(line[2:].strip())}</li>")
        else:
            close_ul()
            out.append(f"<p>{inline_md(line)}</p>")
        i += 1
    close_ul()
    close_table()
    return "\n".join(out)


def split_sections(md: str) -> list[tuple[str, str]]:
    parts = re.split(r"(?m)^(## .+)$", md)
    if not parts:
        return []
    sections: list[tuple[str, str]] = []
    preamble = parts[0].strip()
    if preamble:
        # drop leading H1 for page title handling
        preamble = re.sub(r"(?m)^# .+\n?", "", preamble, count=1).strip()
        if preamble:
            overview = "概述" if "命名空间" in preamble or "调用说明" in preamble else "Overview"
            sections.append((overview, preamble))
    for i in range(1, len(parts), 2):
        title = parts[i][3:].strip()
        body = parts[i + 1].strip() if i + 1 < len(parts) else ""
        sections.append((title, body))
    return sections


def page_html(lang: str, title: str, nav: list[tuple[str, str, bool]], body: str, other_href: str) -> str:
    nav_html = []
    for href, label, active in nav:
        cls = ' class="active"' if active else ""
        nav_html.append(f'<a href="{href}"{cls}>{html.escape(label)}</a>')
    en_cls = ' class="active"' if lang == "en" else ""
    zh_cls = ' class="active"' if lang == "zh" else ""
    return f"""<!DOCTYPE html>
<html lang="{lang}">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>{html.escape(title)} — pitlane API</title>
<link rel="stylesheet" href="../assets/style.css"/>
</head>
<body>
<div class="layout">
  <aside class="side">
    <h1>pitlane API</h1>
    <div class="lang">
      <a href="{other_href if lang == "zh" else "#"}"{en_cls}>English</a>
      <a href="{other_href if lang == "en" else "#"}"{zh_cls}>中文</a>
    </div>
    <nav>
      {"".join(nav_html)}
    </nav>
  </aside>
  <main class="main">
    <div class="meta">Static API guide · open a section from the left</div>
    {body}
  </main>
</div>
</body>
</html>
"""


def collect_files(src: Path) -> list[tuple[str, str, str]]:
    sections = split_sections(src.read_text(encoding="utf-8"))
    files: list[tuple[str, str, str]] = []
    for title, body in sections:
        name = "index.html" if title in ("Overview", "概述", "简介") else f"{slugify(title)}.html"
        files.append((name, title, body))
    if files and files[0][0] != "index.html":
        files[0] = ("index.html", files[0][1], files[0][2])
    return files


def write_lang(lang: str, files: list[tuple[str, str, str]], peers: list[str]) -> None:
    out_dir = OUT / lang
    out_dir.mkdir(parents=True, exist_ok=True)
    other_lang = "zh" if lang == "en" else "en"
    for idx, (name, title, body) in enumerate(files):
        nav = [(n2, t2, n2 == name) for n2, t2, _ in files]
        # Pair EN/ZH by section order so the language switch lands on the matching topic.
        if idx < len(peers):
            other_href = f"../{other_lang}/{peers[idx]}"
        else:
            other_href = f"../{other_lang}/index.html"
        content = f"<h1>{html.escape(title)}</h1>\n" + render_body(body)
        (out_dir / name).write_text(
            page_html(lang, title, nav, content, other_href), encoding="utf-8"
        )


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "assets").mkdir(exist_ok=True)
    (OUT / "assets" / "style.css").write_text(CSS, encoding="utf-8")
    en_files = collect_files(ROOT / "API.md")
    zh_files = collect_files(ROOT / "API_CN.md")
    write_lang("en", en_files, [n for n, _, _ in zh_files])
    write_lang("zh", zh_files, [n for n, _, _ in en_files])
    (OUT / "index.html").write_text(
        """<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"/>
<meta http-equiv="refresh" content="0; url=en/index.html"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>pitlane API</title>
<style>
body{font-family:system-ui,sans-serif;max-width:40rem;margin:2rem auto;padding:0 1rem;line-height:1.5}
code{background:#f4f4f4;padding:0.1em 0.35em}
.note{border-left:3px solid #888;padding-left:0.75rem;color:#444;margin-top:1.5rem}
</style>
</head>
<body>
<p><a href="en/index.html">English</a> · <a href="zh/index.html">中文</a></p>
<p class="note">Browsing this file on <code>github.com/.../blob/...</code> shows source only.
GitHub does not run HTML there. Open a local clone (<code>doc/site/index.html</code>),
an installed <code>share/doc/pitlane/site/</code>, or the GitHub Pages deployment for this repo.</p>
</body></html>
""",
        encoding="utf-8",
    )
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
