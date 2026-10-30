#!/usr/bin/env python3
"""
Writes the skin prototypes: one SVG per design and orientation, for an iPhone
15 sized screen (393 x 852 points, 3 pixels per point), with the controls where
port/src/ios/touchlayout.m puts them. The layout below is a copy of that file's
phone sizes and arithmetic; if the layout changes, change it here and run again:
    python3 port/design/skins/generate.py
"""

import math
import os
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))

GBA_W, GBA_H = 240, 160
PX = 3  # pixels per point

# kPhone in touchlayout.m
MARGIN, GAP, DPAD, FACE, FACE_DX, FACE_DY = 12, 16, 132, 64, 68, 36
SHOULDER, SHOULDER_BAR, START, START_GAP, MENU = (88, 44), 140, (76, 44), 8, 44

# The screen: bounds and safe area insets (top, left, bottom, right).
PORTRAIT = dict(bounds=(393, 852), safe=(59, 0, 34, 0))
LANDSCAPE = dict(bounds=(852, 393), safe=(0, 59, 21, 59))


@dataclass
class Rect:
    x: float
    y: float
    w: float
    h: float

    @property
    def cx(self):
        return self.x + self.w / 2

    @property
    def cy(self):
        return self.y + self.h / 2


def circle(cx, cy, d):
    return Rect(cx - d / 2, cy - d / 2, d, d)


def whole_scale(w, h):
    return max(int(math.floor(min(w * PX / GBA_W, h * PX / GBA_H))), 1)


def layout(portrait):
    bw, bh = (PORTRAIT if portrait else LANDSCAPE)["bounds"]
    st, sl, sb, sr = (PORTRAIT if portrait else LANDSCAPE)["safe"]
    safe = Rect(sl, st, bw - sl - sr, bh - st - sb)
    c = {}
    if portrait:
        controls = 2 * MARGIN + SHOULDER[1] + 2 * GAP + max(DPAD, FACE_DY + FACE) + MENU + MARGIN + START[1]
        split = max(min(bh / 2, safe.y + safe.h - controls), safe.y)
        picture = Rect(safe.x, safe.y, safe.w, split - safe.y)
        left, right = safe.x + MARGIN, safe.x + safe.w - MARGIN
        top, bottom = split + MARGIN, safe.y + safe.h - MARGIN
        centre = safe.cx
        bar = min(SHOULDER_BAR, (right - left - GAP) / 2)
        c["L"] = Rect(left, top, bar, SHOULDER[1])
        c["R"] = Rect(right - bar, top, bar, SHOULDER[1])
        row = bottom - START[1]
        c["Select"] = Rect(centre - START_GAP / 2 - START[0], row, *START)
        c["Start"] = Rect(centre + START_GAP / 2, row, *START)
        c["Menu"] = Rect(centre - MENU / 2, row - MARGIN - MENU, MENU, MENU)
        middle = (top + SHOULDER[1] + GAP + row - GAP) / 2
        c["Dpad"] = circle(left + DPAD / 2, middle, DPAD)
        face = right - (FACE_DX + FACE) / 2
        c["A"] = circle(face + FACE_DX / 2, middle - FACE_DY / 2, FACE)
        c["B"] = circle(face - FACE_DX / 2, middle + FACE_DY / 2, FACE)
    else:
        split = None
        column = max(DPAD, FACE_DX + FACE) + 1.5 * MARGIN
        scale = whole_scale(safe.w - 2 * column, safe.h)
        width = GBA_W * scale / PX
        column = (safe.w - width) / 2
        picture = Rect(safe.x + column, safe.y, width, safe.h)
        top, bottom = safe.y + MARGIN, safe.y + safe.h - MARGIN
        left, right = safe.x, safe.x + safe.w
        lc, rc = left + column / 2, right - column / 2
        c["L"] = Rect(left + MARGIN, top, *SHOULDER)
        c["R"] = Rect(right - MARGIN - SHOULDER[0], top, *SHOULDER)
        region = top + SHOULDER[1] + GAP
        stack = DPAD + GAP + 2 * START[1] + START_GAP
        stack_top = region + max(0, (bottom - region - stack) / 2)
        dy = stack_top + DPAD / 2
        c["Dpad"] = circle(lc, dy, DPAD)
        st_top = stack_top + DPAD + GAP
        c["Start"] = Rect(lc - START[0] / 2, st_top, *START)
        c["Select"] = Rect(lc - START[0] / 2, st_top + START[1] + START_GAP, *START)
        c["A"] = circle(rc + FACE_DX / 2, dy - FACE_DY / 2, FACE)
        c["B"] = circle(rc - FACE_DX / 2, dy + FACE_DY / 2, FACE)
        c["Menu"] = Rect(right - MARGIN - MENU, bottom - MENU, MENU, MENU)
    scale = whole_scale(picture.w, picture.h)
    iw, ih = GBA_W * scale / PX, GBA_H * scale / PX
    image = Rect(picture.cx - iw / 2, picture.cy - ih / 2, iw, ih)
    return dict(bounds=(bw, bh), safe=safe, picture=picture, image=image, scale=scale, controls=c, split=split)


# Designs: colours of each layer. "outline" draws strokes only.
DESIGNS = {
    "indigo": dict(
        title="Indigo", note="Classic purple plastic, dark caps, lavender print.",
        shell=("#6a5fc4", "#4a3fa3", "#372e85"), edge="#2a226a", highlight="#8e84dc", well="#3b3289",
        lens=("#353747", "#22232e"), cutout_mark="#9aa0ff", led="#ff5a4e",
        dpad="#2b2b35", face="#3c3c4b", face_hi="#5b5b70", small="#2b2b35", menu="#3b3289",
        print_="#d5d0f6", cap_text="#9e9db4", grille="#271f66",
    ),
    "glacier": dict(
        title="Glacier", note="Clear ice-blue plastic; the parts inside show through.",
        shell=("#d7ecfb", "#a9cdee", "#86b2df"), edge="#5f8fc0", highlight="#ffffff", well="#7fa8d6",
        lens=("#2a3442", "#1a212b"), cutout_mark="#7fd3ff", led="#3ad07a",
        dpad="#e9f4fd", face="#f4f9fe", face_hi="#ffffff", small="#d4e7f8", menu="#bfdaf2",
        print_="#23466e", cap_text="#5f86ad", grille="#4d7cae",
        shell_opacity=0.72, internals="#4f7fae",
    ),
    "onyx": dict(
        title="Onyx", note="Matte black with a copper print and copper face buttons.",
        shell=("#34343a", "#222227", "#151518"), edge="#08080a", highlight="#4b4b53", well="#0f0f12",
        lens=("#121214", "#060607"), cutout_mark="#d39a6a", led="#d39a6a",
        dpad="#0b0b0d", face="#b9744a", face_hi="#dc9a6c", small="#0b0b0d", menu="#1b1b1f",
        print_="#d39a6a", cap_text="#6b5546", grille="#0a0a0c",
    ),
    "outline": dict(
        title="Outline", note="No plastic: a single-weight line drawing over the black screen.",
        outline="#8f88d8",
    ),
}

FONT = "-apple-system, 'SF Pro Rounded', 'Helvetica Neue', Arial, sans-serif"


def f(v):
    return f"{v:.1f}".rstrip("0").rstrip(".")


def rect(r, rx=0, **attrs):
    extra = "".join(f' {k.replace("_", "-")}="{v}"' for k, v in attrs.items())
    return f'<rect x="{f(r.x)}" y="{f(r.y)}" width="{f(r.w)}" height="{f(r.h)}" rx="{f(rx)}"{extra}/>'


def circ(cx, cy, r, **attrs):
    extra = "".join(f' {k.replace("_", "-")}="{v}"' for k, v in attrs.items())
    return f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(r)}"{extra}/>'


def text(x, y, s, size, fill, weight=700, spacing=0.06, anchor="middle"):
    return (f'<text x="{f(x)}" y="{f(y)}" font-family="{FONT}" font-size="{f(size)}" font-weight="{weight}" '
            f'letter-spacing="{spacing}em" text-anchor="{anchor}" fill="{fill}">{s}</text>')


def inset(r, d):
    return Rect(r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d)


def cross_path(cx, cy, size, arm):
    h, a = size / 2, arm / 2
    pts = [(-a, -h), (a, -h), (a, -a), (h, -a), (h, a), (a, a), (a, h), (-a, h), (-a, a), (-h, a), (-h, -a), (-a, -a)]
    return "M" + " L".join(f"{f(cx + x)} {f(cy + y)}" for x, y in pts) + " Z"


def shoulder_path(r, outer_left, rx_out=24, rx_in=10):
    # A shoulder with a large radius on its outer top corner.
    x0, y0, x1, y1 = r.x, r.y, r.x + r.w, r.y + r.h
    tl, tr = (rx_out, rx_in) if outer_left else (rx_in, rx_out)
    bl = br = rx_in
    return (f"M{f(x0 + tl)} {f(y0)} H{f(x1 - tr)} Q{f(x1)} {f(y0)} {f(x1)} {f(y0 + tr)} V{f(y1 - br)} "
            f"Q{f(x1)} {f(y1)} {f(x1 - br)} {f(y1)} H{f(x0 + bl)} Q{f(x0)} {f(y1)} {f(x0)} {f(y1 - bl)} "
            f"V{f(y0 + tl)} Q{f(x0)} {f(y0)} {f(x0 + tl)} {f(y0)} Z")


def grille(cx, cy, fill, stroke=None):
    # Six slanted rows of holes, longest in the middle.
    out = []
    counts = [3, 5, 6, 6, 5, 3]
    for i, n in enumerate(counts):
        oy = (i - 2.5) * 9
        for j in range(n):
            ox = (j - (n - 1) / 2) * 9
            # slant the rows
            x, y = cx + ox + oy * 0.45, cy + oy - ox * 0.25
            if stroke:
                out.append(circ(x, y, 2.4, fill="none", stroke=stroke, stroke_width=1))
            else:
                out.append(circ(x, y, 2.6, fill=fill))
    return "\n    ".join(out)


def svg(design_key, portrait):
    d = DESIGNS[design_key]
    L = layout(portrait)
    bw, bh = L["bounds"]
    c = L["controls"]
    img = L["image"]
    line = d.get("outline")
    orient = "portrait" if portrait else "landscape"
    p = f"{design_key}-{orient}-"  # id prefix, so several SVGs can share a page
    A, B = c["A"], c["B"]
    ang = math.degrees(math.atan2(A.cy - B.cy, A.cx - B.cx))
    dist = math.hypot(A.cx - B.cx, A.cy - B.cy)
    lens = Rect(img.x - 14, img.y - 16, img.w + 28, img.h + 32)
    grille_at = (338, 735) if portrait else (680, 292)

    def stroke(w=1.5):
        return dict(fill="none", stroke=line, stroke_width=w)

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {bw} {bh}" width="{bw}" height="{bh}">',
         f"<title>{d['title']} skin, {orient}, {bw}x{bh} pt</title>"]
    if not line:
        s0, s1, s2 = d["shell"]
        o.append(f"""<defs>
  <linearGradient id="{p}shell" x1="0" y1="0" x2="0.3" y2="1">
    <stop offset="0" stop-color="{s0}"/><stop offset="0.55" stop-color="{s1}"/><stop offset="1" stop-color="{s2}"/>
  </linearGradient>
  <linearGradient id="{p}lens" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="{d['lens'][0]}"/><stop offset="1" stop-color="{d['lens'][1]}"/>
  </linearGradient>
  <radialGradient id="{p}face" cx="0.38" cy="0.32" r="0.75">
    <stop offset="0" stop-color="{d['face_hi']}"/><stop offset="1" stop-color="{d['face']}"/>
  </radialGradient>
  <filter id="{p}drop" x="-20%" y="-20%" width="140%" height="140%">
    <feDropShadow dx="0" dy="2" stdDeviation="1.6" flood-color="#000" flood-opacity="0.45"/>
  </filter>
  <filter id="{p}sunk" x="-10%" y="-10%" width="120%" height="120%">
    <feOffset dy="1.5"/><feGaussianBlur stdDeviation="1.5" result="b"/>
    <feComposite in="SourceGraphic" in2="b" operator="arithmetic" k2="1" k3="-0.35"/>
  </filter>
</defs>""")

    # 1. shell: the plastic around everything, and its seams.
    o.append('<g id="shell">')
    if line:
        o.append("  " + rect(Rect(0.75, 0.75, bw - 1.5, bh - 1.5), 55, **stroke(1.5)))
        o.append("  " + rect(Rect(8, 8, bw - 16, bh - 16), 48, **stroke(0.75), stroke_dasharray="2 4"))
        if portrait:
            o.append(f'  <path d="M8 {f(L["split"])} H{bw - 8}" fill="none" stroke="{line}" stroke-width="0.75"/>')
    else:
        op = d.get("shell_opacity", 1)
        o.append("  " + rect(Rect(0, 0, bw, bh), 55, fill=f"url(#{p}shell)", fill_opacity=op))
        if d.get("internals"):
            o.append(f'  <g id="internals" fill="none" stroke="{d["internals"]}" stroke-opacity="0.35" stroke-width="1.2">')
            board = Rect(16, L["split"] + 6, bw - 32, bh - L["split"] - 40) if portrait else Rect(18, 30, bw - 36, bh - 50)
            o.append("    " + rect(board, 20))
            for k, r in (("Dpad", c["Dpad"]), ("A", A), ("B", B)):
                o.append("    " + circ(r.cx, r.cy, r.w / 2 + 14))
            tx, ty = c["Dpad"].cx, c["Dpad"].cy
            o.append(f'    <path d="M{f(tx + 80)} {f(ty)} H{f(B.cx - 50)} M{f(c["Select"].cx)} {f(c["Select"].y - 10)} '
                     f'V{f(ty + 90 if portrait else ty + 70)}"/>')
            o.append("  </g>")
        o.append("  " + rect(Rect(1, 1, bw - 2, bh - 2), 54, fill="none", stroke=d["edge"], stroke_width=2))
        o.append("  " + rect(Rect(7, 7, bw - 14, bh - 14), 49, fill="none", stroke=d["highlight"],
                             stroke_opacity=0.35, stroke_width=1))
        if portrait:  # the hinge between the halves, as the clamshell
            y = L["split"]
            o.append("  " + rect(Rect(0, y - 4, bw, 8), 0, fill=d["edge"], fill_opacity=0.55))
            o.append(f'  <path d="M0 {f(y - 4)} H{bw}" stroke="{d["highlight"]}" stroke-opacity="0.4"/>')
    o.append("</g>")

    # 2. grille: the speaker holes.
    o.append('<g id="grille">')
    o.append("    " + grille(*grille_at, d.get("grille"), stroke=line))
    o.append("</g>")

    # 3. lens: the dark glass around the picture, with the power light.
    o.append('<g id="lens">')
    if line:
        o.append("  " + rect(lens, 14, **stroke(1.5)))
        o.append("  " + circ(lens.x + 10, lens.y + 8, 2.5, **stroke(1)))
    else:
        o.append("  " + rect(lens, 14, fill=f"url(#{p}lens)", stroke=d["edge"], stroke_width=1.5))
        o.append("  " + circ(lens.x + 10, lens.y + 8, 2.6, fill=d["led"]))
        o.append("  " + circ(lens.x + 10, lens.y + 8, 5, fill=d["led"], fill_opacity=0.25))
    o.append("</g>")

    # 4. cutout: where the game's picture shows. Drawn black; the app leaves it out.
    o.append(f'<g id="cutout" data-gba="240x160" data-scale="{L["scale"]}">')
    o.append("  " + rect(img, 0, fill="#000"))
    o.append("  " + rect(img, 0, fill="none", stroke=d.get("cutout_mark", line), stroke_width=1,
                         stroke_dasharray="6 4"))
    o.append("  " + text(img.cx, img.cy - 4, "240 × 160", 16, d.get("cutout_mark", line), 600, 0.04))
    o.append("  " + text(img.cx, img.cy + 16, f"×{L['scale']} = {GBA_W * L['scale']} × {GBA_H * L['scale']} px",
                         11, d.get("cutout_mark", line), 500, 0.04))
    o.append("</g>")

    # 5. wells: the recesses the caps sit in.
    o.append('<g id="wells">')
    well = stroke(1) if line else dict(fill=d["well"], filter=f"url(#{p}sunk)")
    o.append("  " + circ(c["Dpad"].cx, c["Dpad"].cy, DPAD / 2 + 7, **well))
    mx, my = (A.cx + B.cx) / 2, (A.cy + B.cy) / 2
    pill = Rect(mx - (dist + FACE + 16) / 2, my - (FACE + 16) / 2, dist + FACE + 16, FACE + 16)
    o.append(f'  <g transform="rotate({f(ang)} {f(mx)} {f(my)})">' + rect(pill, pill.h / 2, **well) + "</g>")
    for k in ("Start", "Select"):
        r = c[k]
        o.append("  " + rect(Rect(r.cx - 30, r.y + 5, 60, 22), 11, **well))
    o.append("</g>")

    # 6. caps: the buttons, one group each, the size of its touch target.
    o.append('<g id="caps">')
    cap = (lambda col: stroke(1.5)) if line else (lambda col: dict(fill=col, filter=f"url(#{p}drop)"))
    D = c["Dpad"]
    o.append(f'  <g id="cap-dpad"><path d="{cross_path(D.cx, D.cy, DPAD * 0.9, DPAD * 0.33)}" '
             + " ".join(f'{k.replace("_", "-")}="{v}"' for k, v in cap(d.get("dpad")).items()) + "/>")
    o.append("    " + circ(D.cx, D.cy, DPAD * 0.09, **(stroke(1) if line else dict(fill="#000", fill_opacity=0.25))))
    tri = DPAD * 0.36
    for ang_t in (0, 90, 180, 270):
        rad = math.radians(ang_t)
        tx, ty = D.cx + tri * math.cos(rad), D.cy + tri * math.sin(rad)
        a1, a2 = math.radians(ang_t + 140), math.radians(ang_t - 140)
        pts = [(tx + 6 * math.cos(rad), ty + 6 * math.sin(rad)), (tx + 6 * math.cos(a1), ty + 6 * math.sin(a1)),
               (tx + 6 * math.cos(a2), ty + 6 * math.sin(a2))]
        fill = "none" if line else d["cap_text"]
        o.append(f'    <path d="M{" L".join(f"{f(x)} {f(y)}" for x, y in pts)} Z" fill="{fill}"'
                 + (f' stroke="{line}"' if line else "") + "/>")
    o.append("  </g>")
    for k in ("A", "B"):
        r = c[k]
        style = stroke(1.5) if line else dict(fill=f"url(#{p}face)", filter=f"url(#{p}drop)")
        o.append(f'  <g id="cap-{k.lower()}">' + circ(r.cx, r.cy, FACE / 2, **style)
                 + text(r.cx, r.cy + 7, k, 20, line or d["cap_text"], 700, 0) + "</g>")
    for k in ("Start", "Select"):
        r = c[k]
        o.append(f'  <g id="cap-{k.lower()}">' + rect(Rect(r.cx - 24, r.y + 9, 48, 14), 7, **cap(d.get("small")))
                 + "</g>")
    for k in ("L", "R"):
        r = c[k]
        if portrait:
            shape = f'<rect x="{f(r.x)}" y="{f(r.y)}" width="{f(r.w)}" height="{f(r.h)}" rx="12" '
        else:
            shape = f'<path d="{shoulder_path(r, k == "L")}" '
        style = cap(d.get("small") if line else d["well"])
        o.append(f'  <g id="cap-{k.lower()}">' + shape + " ".join(f'{a.replace("_", "-")}="{v}"' for a, v in style.items())
                 + "/></g>")
    m = c["Menu"]
    o.append(f'  <g id="cap-menu">' + circ(m.cx, m.cy, 17, **cap(d.get("menu")))
             + "".join(f'<rect x="{f(m.cx - 8)}" y="{f(m.cy + dy - 1)}" width="16" height="2" rx="1" '
                       f'fill="{line or d["print_"]}"/>' for dy in (-5, 0, 5)) + "</g>")
    o.append("</g>")

    # 7. labels: the print on the shell.
    ink = line or d["print_"]
    o.append('<g id="labels">')
    for k in ("Start", "Select"):
        r = c[k]
        o.append("  " + text(r.cx, r.y + r.h - 4, k.upper(), 10, ink, 700, 0.14))
    for k in ("L", "R"):
        r = c[k]
        o.append("  " + text(r.cx, r.cy + 6, k, 17, ink, 700, 0))
    o.append("</g>")

    # Not drawn: the touch targets from touchlayout.m, to check the caps against.
    o.append('<g id="touch-targets" display="none" fill="none" stroke="#ff2d55" stroke-width="1">')
    for k, r in c.items():
        o.append(f"  " + (circ(r.cx, r.cy, r.w / 2) if k in ("Dpad", "A", "B") else rect(r, 0)))
    o.append("</g>")
    o.append("</svg>")
    return "\n".join(o) + "\n"


def main():
    for key in DESIGNS:
        for portrait in (True, False):
            name = f"{key}-{'portrait' if portrait else 'landscape'}.svg"
            with open(os.path.join(HERE, name), "w") as out:
                out.write(svg(key, portrait))


if __name__ == "__main__":
    main()
