#!/usr/bin/env python3
"""build-site.py - assemble the website: the landing page and the user guide.

    python3 tools/build-site.py [out-dir]        default: _site

The guide is written in Markdown under docs/ - readable as is on GitHub - and
rendered here into pages that share the landing page's look. The install page
IS docs/INSTALL.md: one text, two places to read it. Needs `markdown`
(pip install markdown==3.11); everything else is the standard library.

Links between guide pages are written as links between the .md files, so they
work on GitHub too; this script turns them into links between the .html pages.
A link to any other file of the repository becomes a link to that file on
GitHub, and a screenshot from docs/screenshots/ is served from the site itself.
"""
import html
import os
import posixpath
import re
import shutil
import sys

import markdown

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GITHUB = "https://github.com/Wasabules/Halyard"

# (slug, source, title in the navigation, one-line summary for the docs home)
GUIDE = [
    ("install", "docs/INSTALL.md", "Install and sign in",
     "Put Halyard on your console, link it to your Shadow account, move from shadow-client."),
    ("using", "docs/guide/using.md", "Using Halyard",
     "The machine list, connecting, the stream, the pause menu, leaving."),
    ("controls", "docs/guide/controls.md", "Controls",
     "Buttons, the mouse, the keyboard and touch gestures, on each console."),
    ("settings", "docs/guide/settings.md", "Settings",
     "Every setting, what it changes, and its default."),
    ("performance", "docs/guide/performance.md", "Picture, sound and network",
     "Getting a clean picture, reading the performance panel, Wi-Fi versus wired."),
    ("faq", "docs/guide/faq.md", "FAQ and troubleshooting",
     "Short answers to the questions people ask, and what to do when something is wrong."),
    ("advanced", "docs/guide/advanced.md", "Advanced",
     "The env.txt file, the log, and the other files in the data directory."),
]
SOURCE_TO_SLUG = {src: slug for slug, src, _, _ in GUIDE}


def rewrite_links(body, source):
    """Relative links of the Markdown source -> links that work on the site."""
    base = posixpath.dirname(source)

    def fix(m):
        attr, url = m.group(1), m.group(2)
        if re.match(r"^[a-z][a-z0-9+.-]*:", url) or url.startswith("#") or url.startswith("//"):
            return m.group(0)
        path, _, anchor = url.partition("#")
        target = posixpath.normpath(posixpath.join(base, path)) if path else source
        if target in SOURCE_TO_SLUG:
            new = SOURCE_TO_SLUG[target] + ".html"
        elif target.startswith("docs/screenshots/"):
            new = "../assets/screens/" + posixpath.basename(target)
        else:
            kind = "tree" if os.path.isdir(os.path.join(REPO, target)) else "blob"
            new = "%s/%s/main/%s" % (GITHUB, kind, target)
        if anchor:
            new += "#" + anchor
        return '%s="%s"' % (attr, html.escape(new, quote=True))

    return re.sub(r'(href|src)="([^"]+)"', fix, body)


def render(source):
    text = open(os.path.join(REPO, source), encoding="utf-8").read()
    md = markdown.Markdown(extensions=["tables", "fenced_code", "attr_list", "sane_lists",
                                       "toc"],
                           extension_configs={"toc": {"permalink": "#", "permalink_title": ""}})
    body = md.convert(text)
    return rewrite_links(body, source)


PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>{title}</title>
<meta name="description" content="{description}">
<meta name="theme-color" content="#121318">
<link rel="icon" type="image/png" href="../assets/logo-64.png">
<link rel="stylesheet" href="docs.css">
</head>
<body>
<header class="topbar">
  <a class="brand" href="../"><img src="../assets/logo-64.png" width="28" height="28" alt=""><span>Halyard</span></a>
  <a class="section" href="./">Docs</a>
  <nav aria-label="Project">
    <a href="{github}/releases/latest">Download</a>
    <a href="{github}">GitHub</a>
  </nav>
</header>
<div class="layout">
  <nav class="sidebar" aria-label="Guide">
    <p class="side-label">Guide</p>
    <ul>
{nav}
    </ul>
  </nav>
  <main class="content">
{body}
{pager}
  </main>
</div>
<footer class="foot">
  <span>Halyard is free software under the GPL-3.0-or-later. Unofficial: not affiliated with Shadow, Nintendo or Sony.</span>
  <a href="{github}/blob/main/{source}">Edit this page on GitHub</a>
</footer>
</body>
</html>
"""


def nav_html(active):
    out = []
    for slug, _, title, _ in GUIDE:
        cur = ' aria-current="page"' if slug == active else ""
        out.append('      <li><a href="%s.html"%s>%s</a></li>' % (slug, cur, html.escape(title)))
    return "\n".join(out)


def pager_html(i):
    parts = ['<nav class="pager" aria-label="Next and previous pages">']
    if i > 0:
        s, _, t, _ = GUIDE[i - 1]
        parts.append('<a class="prev" href="%s.html"><span>Previous</span>%s</a>' % (s, html.escape(t)))
    else:
        parts.append("<span></span>")
    if i + 1 < len(GUIDE):
        s, _, t, _ = GUIDE[i + 1]
        parts.append('<a class="next" href="%s.html"><span>Next</span>%s</a>' % (s, html.escape(t)))
    parts.append("</nav>")
    return "\n".join(parts)


def docs_home():
    cards = "\n".join(
        '<li><a href="%s.html"><strong>%s</strong><span>%s</span></a></li>'
        % (s, html.escape(t), html.escape(d)) for s, _, t, d in GUIDE)
    return """<h1>Halyard documentation</h1>
<p class="lede">Everything you need to use Halyard: installing it, the screens, the controls on
each console, every setting, and what to do when the picture or the network misbehaves.</p>
<ul class="cards">
%s
</ul>""" % cards


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "_site"))
    if os.path.exists(out):
        shutil.rmtree(out)
    shutil.copytree(os.path.join(REPO, "site"), out)

    assets = os.path.join(out, "assets")
    screens = os.path.join(assets, "screens")
    os.makedirs(screens, exist_ok=True)
    shutil.copy(os.path.join(REPO, "resources/img/background.jpg"), assets)
    for n in (64, 128, 192, 512):
        shutil.copy(os.path.join(REPO, "clients/borealis/branding/png/logo-%dx%d.png" % (n, n)),
                    os.path.join(assets, "logo-%d.png" % n))
    shot_dir = os.path.join(REPO, "docs/screenshots")
    for f in sorted(os.listdir(shot_dir)):
        if f.endswith(".webp"):
            shutil.copy(os.path.join(shot_dir, f), screens)

    docs = os.path.join(out, "docs")
    os.makedirs(docs, exist_ok=True)
    shutil.move(os.path.join(out, "docs.css"), os.path.join(docs, "docs.css"))

    def write(name, title, description, body, source, active, pager=""):
        page = PAGE.format(title=html.escape(title), description=html.escape(description),
                           github=GITHUB, nav=nav_html(active), body=body, pager=pager,
                           source=source)
        with open(os.path.join(docs, name), "w", encoding="utf-8") as f:
            f.write(page)

    write("index.html", "Halyard documentation",
          "How to install and use Halyard, the unofficial Shadow client for Switch and PS Vita.",
          docs_home(), "docs/guide", "")
    for i, (slug, source, title, blurb) in enumerate(GUIDE):
        write(slug + ".html", "%s - Halyard documentation" % title, blurb,
              render(source), source, slug, pager_html(i))

    count = sum(len(fs) for _, _, fs in os.walk(out))
    print("site written to %s (%d files, %d guide pages)" % (out, count, len(GUIDE)))


if __name__ == "__main__":
    main()
