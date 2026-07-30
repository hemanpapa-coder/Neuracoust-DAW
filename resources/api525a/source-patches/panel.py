"""Faceplate (back board) construction."""
import math
import numpy as np
from PIL import Image
from common import *
from knobs import render_svg

F = FONT


def T(x, y, s, size, fill, anchor="middle", ls=0.0, weight="bold", op=1.0):
    a = {"middle": "middle", "start": "start", "end": "end"}[anchor]
    extra = f' letter-spacing="{ls}"' if ls else ""
    o = f' fill-opacity="{op}"' if op != 1.0 else ""
    return (f'<text x="{x:.2f}" y="{y:.2f}" font-family="{F}" font-weight="{weight}" '
            f'font-size="{size:.2f}" fill="{fill}" text-anchor="{a}"{extra}{o}'
            f' xml:space="preserve">{s}</text>')


def dots(cx, cy, r, angles, rad=2.25, fill=CYAN):
    out = []
    for a in angles:
        x, y = polar(cx, cy, r, a)
        out.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{rad}" fill="{fill}"/>')
    return "".join(out)


SMALL_DOT_ANGLES = [-150, -120, -90, -60, -30, 30, 60, 90, 120, 150]
CEIL_DOT_ANGLES = [-120, -60, 0, 60, 120]


def api_logo():
    """Two op-amp triangles + leads, as on the reference panel."""
    p = []
    # leads
    p.append('<rect x="13.0" y="14.3" width="10.5" height="3.1" fill="%s"/>' % CYAN)
    p.append('<rect x="35.0" y="13.9" width="11.6" height="3.1" fill="%s"/>' % CYAN)
    p.append('<rect x="18.6" y="25.6" width="11.0" height="3.1" fill="%s"/>' % CYAN)
    p.append('<rect x="41.0" y="26.2" width="8.6" height="3.1" fill="%s"/>' % CYAN)
    # lower triangle (drawn first, upper overlaps it)
    p.append('<path d="M22.6,17.0 L28.3,38.6 L45.2,28.3 Z" fill="%s"/>' % CYAN)
    # notch that separates the two triangles
    p.append('<path d="M20.4,4.6 L25.2,26.6 L40.0,15.6 Z" fill="%s"/>' % CYAN)
    return "".join(p)


def screw(cx, cy, r=SCREW_R, lit=True):
    s = (f'<circle cx="{cx}" cy="{cy}" r="{r*1.55:.2f}" fill="#000000" fill-opacity="0.85"/>'
         f'<circle cx="{cx}" cy="{cy}" r="{r*1.30:.2f}" fill="#0a070c"/>')
    if lit:
        s += (f'<circle cx="{cx}" cy="{cy}" r="{r*0.92:.2f}" fill="#f4f2f6"/>'
              f'<circle cx="{cx}" cy="{cy}" r="{r*0.92:.2f}" fill="url(#gScrew)"/>')
    else:
        s += (f'<circle cx="{cx}" cy="{cy}" r="{r*0.92:.2f}" fill="#050308"/>'
              f'<circle cx="{cx}" cy="{cy-r*0.15:.2f}" r="{r*0.92:.2f}" fill="url(#gHole)"/>')
    return s


def meter_face(with_needle_at=None):
    """Mechanical GR meter: bezel + scale plate. Returns svg string."""
    bx, by, bw, bh = METER_BEZEL
    px, py, pw, ph = METER_PLATE
    s = []
    s.append(f'<rect x="{bx-1.2}" y="{by-1.2}" width="{bw+2.4}" height="{bh+2.4}" rx="2.2" '
             f'fill="#000000" fill-opacity="0.75"/>')
    s.append(f'<rect x="{bx}" y="{by}" width="{bw}" height="{bh}" rx="1.8" fill="url(#gBezel)"/>')
    s.append(f'<rect x="{bx+2.4}" y="{by+2.4}" width="{bw-4.8}" height="{bh-4.8}" rx="1.2" '
             f'fill="#1a1820"/>')
    # scale plate
    s.append(f'<rect x="{px}" y="{py}" width="{pw}" height="{ph}" fill="url(#gPlate)"/>')
    # numbers
    labs = [("20", 61.6), ("10", 71.2), ("6", 80.6), ("4", 89.2), ("2", 101.4), ("0", 113.4)]
    for txt, x in labs:
        s.append(T(x, py + 10.6, txt, 10.2, "#101014", "middle"))
    # black tick blocks
    blocks = [(57.6, 11.0), (76.0, 9.4), (91.6, 12.2), (112.8, 11.6)]
    for x, w in blocks:
        s.append(f'<rect x="{x}" y="{py+12.4}" width="{w}" height="{4.6}" fill="#101014"/>')
    s.append(T(px + pw / 2 + 0.5, py + 27.6, "GAIN REDUCTION", 8.0, "#101014", "middle"))
    s.append(f'<rect x="{px+2.4}" y="{py+30.4}" width="{4.4}" height="{3.4}" fill="#101014"/>')
    s.append(f'<rect x="{px+62.0}" y="{py+30.4}" width="{4.0}" height="{3.4}" fill="#101014"/>')
    # plate shading + glass
    s.append(f'<rect x="{px}" y="{py}" width="{pw}" height="{ph}" fill="url(#gPlateShade)"/>')
    if with_needle_at is not None:
        s.append(needle_svg(with_needle_at))
    s.append(f'<rect x="{px}" y="{py}" width="{pw}" height="{ph}" fill="url(#gGlass)"/>')
    s.append(f'<rect x="{bx}" y="{by}" width="{bw}" height="{bh}" rx="1.8" fill="none" '
             f'stroke="#000" stroke-opacity="0.6" stroke-width="0.7"/>')
    return "".join(s)


def needle_svg(x):
    return (f'<rect x="{x-NEEDLE_W/2:.2f}" y="{NEEDLE_Y0}" width="{NEEDLE_W}" '
            f'height="{NEEDLE_Y1-NEEDLE_Y0}" fill="{NEEDLE}"/>')


def led(cx, cy, r, off_col):
    return (f'<circle cx="{cx}" cy="{cy}" r="{r+0.9:.2f}" fill="#000" fill-opacity="0.8"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{r:.2f}" fill="{off_col}"/>'
            f'<circle cx="{cx-r*0.28:.2f}" cy="{cy-r*0.32:.2f}" r="{r*0.42:.2f}" '
            f'fill="#ffffff" fill-opacity="0.16"/>')


def defs():
    return f'''<defs>
  <linearGradient id="gPanel" x1="0" y1="0" x2="0" y2="{PANEL_H}" gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="#191520"/><stop offset="0.08" stop-color="#100c12"/>
    <stop offset="0.55" stop-color="#0d0a10"/><stop offset="1" stop-color="#120e15"/></linearGradient>
  <linearGradient id="gPanelX" x1="0" y1="0" x2="{PANEL_W}" y2="0" gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="#000000" stop-opacity="0.45"/>
    <stop offset="0.18" stop-color="#000000" stop-opacity="0"/>
    <stop offset="0.85" stop-color="#ffffff" stop-opacity="0.02"/>
    <stop offset="1" stop-color="#ffffff" stop-opacity="0.10"/></linearGradient>
  <radialGradient id="gHole" cx="0.5" cy="0.25" r="0.9">
    <stop offset="0" stop-color="#3a3542" stop-opacity="0.9"/>
    <stop offset="0.55" stop-color="#120f16" stop-opacity="0.7"/>
    <stop offset="1" stop-color="#000000" stop-opacity="0.9"/></radialGradient>
  <radialGradient id="gScrew" cx="0.35" cy="0.3" r="0.85">
    <stop offset="0" stop-color="#ffffff"/><stop offset="0.6" stop-color="#e2dfe8"/>
    <stop offset="1" stop-color="#9a96a4"/></radialGradient>
  <linearGradient id="gBezel" x1="0" y1="{METER_BEZEL[1]}" x2="0" y2="{METER_BEZEL[1]+METER_BEZEL[3]}"
      gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="#5d5a66"/><stop offset="0.12" stop-color="#2a2731"/>
    <stop offset="0.85" stop-color="#232029"/><stop offset="1" stop-color="#6b6874"/></linearGradient>
  <linearGradient id="gPlate" x1="0" y1="{METER_PLATE[1]}" x2="0" y2="{METER_PLATE[1]+METER_PLATE[3]}"
      gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="#d7d5dd"/><stop offset="0.5" stop-color="#c3c1ca"/>
    <stop offset="1" stop-color="#adaab4"/></linearGradient>
  <linearGradient id="gPlateShade" x1="0" y1="{METER_PLATE[1]}" x2="0" y2="{METER_PLATE[1]+METER_PLATE[3]}"
      gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="#000000" stop-opacity="0.30"/>
    <stop offset="0.22" stop-color="#000000" stop-opacity="0"/>
    <stop offset="0.80" stop-color="#000000" stop-opacity="0"/>
    <stop offset="1" stop-color="#000000" stop-opacity="0.28"/></linearGradient>
  <linearGradient id="gGlass" x1="{METER_PLATE[0]}" y1="{METER_PLATE[1]}"
      x2="{METER_PLATE[0]+METER_PLATE[2]}" y2="{METER_PLATE[1]+METER_PLATE[3]}"
      gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="#ffffff" stop-opacity="0.14"/>
    <stop offset="0.35" stop-color="#ffffff" stop-opacity="0.02"/>
    <stop offset="1" stop-color="#ffffff" stop-opacity="0.07"/></linearGradient>
  <linearGradient id="gBtn" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#f3f1f7"/><stop offset="0.45" stop-color="#d3d0da"/>
    <stop offset="1" stop-color="#a5a1af"/></linearGradient>
  <linearGradient id="gBtnDn" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#8d8997"/><stop offset="0.45" stop-color="#adaab7"/>
    <stop offset="1" stop-color="#d3d0da"/></linearGradient>
  <linearGradient id="gBtnUpShade" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#ffffff" stop-opacity="0.18"/>
    <stop offset="0.25" stop-color="#ffffff" stop-opacity="0"/>
    <stop offset="0.70" stop-color="#000000" stop-opacity="0.04"/>
    <stop offset="1" stop-color="#000000" stop-opacity="0.16"/></linearGradient>
  <linearGradient id="gBtnInner" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#000000" stop-opacity="0.42"/>
    <stop offset="0.30" stop-color="#000000" stop-opacity="0.10"/>
    <stop offset="0.72" stop-color="#000000" stop-opacity="0"/>
    <stop offset="1" stop-color="#ffffff" stop-opacity="0.26"/></linearGradient>
</defs>'''


def button_svg(size=BTN_W, down=False, scale=1.0, pad=3.0):
    """Standalone button sprite. down=True is the latched / pressed state."""
    w = size + pad * 2
    off = 1.4 if down else 0.0
    h = size - off
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w*scale:.0f}" height="{w*scale:.0f}" '
         f'viewBox="0 0 {w} {w}">', defs()]
    # socket the cap sits in
    s.append(f'<rect x="{pad-1.2}" y="{pad-1.2}" width="{size+2.4}" height="{size+2.4}" rx="4.2" '
             f'fill="#050308" fill-opacity="0.92"/>')
    if not down:
        s.append(f'<rect x="{pad-0.4}" y="{pad+1.6}" width="{size+0.8}" height="{size}" rx="3.4" '
                 f'fill="#000" fill-opacity="0.5"/>')
    s.append(f'<rect x="{pad}" y="{pad+off}" width="{size}" height="{h}" rx="3.0" '
             f'fill="url(#{"gBtnDn" if down else "gBtn"})"/>')
    s.append(f'<rect x="{pad}" y="{pad+off}" width="{size}" height="{h}" rx="3.0" '
             f'fill="url(#{"gBtnInner" if down else "gBtnUpShade"})"/>')
    # moulded creases, barely there
    for dx in (-3.4, 3.4):
        s.append(f'<rect x="{pad+size/2+dx-0.85:.2f}" y="{pad+off+h*0.30:.2f}" width="1.7" '
                 f'height="{h*0.40:.2f}" rx="0.85" fill="#000000" '
                 f'fill-opacity="{0.13 if down else 0.07}"/>')
    s.append(f'<rect x="{pad+0.45}" y="{pad+off+0.45}" width="{size-0.9}" height="{h-0.9}" rx="2.6" '
             f'fill="none" stroke="#ffffff" stroke-opacity="{0.14 if down else 0.40}" stroke-width="0.9"/>')
    s.append("</svg>")
    return "\n".join(s)


def ceiling_arc():
    cx, cy = CEILING
    r = 80.0
    a0, a1 = 38.0, 150.0
    x0, y0 = polar(cx, cy, r, a0)
    x1, y1 = polar(cx, cy, r, a1)
    d = f"M{x0:.2f},{y0:.2f} A{r},{r} 0 0 1 {x1:.2f},{y1:.2f}"
    out = [f'<path d="{d}" fill="none" stroke="{CYAN}" stroke-width="1.7"/>']
    # arrow heads (tangential)
    for ang, sign in ((a0, -1), (a1, +1)):
        px, py = polar(cx, cy, r, ang)
        t = math.radians(ang)
        tx, ty = math.cos(t) * sign, math.sin(t) * sign      # tangent dir
        nx, ny = math.sin(t), -math.cos(t)                   # outward normal
        L, Wd = 7.4, 3.3
        p1 = (px + tx * L, py + ty * L)
        p2 = (px - nx * Wd, py - ny * Wd)
        p3 = (px + nx * Wd, py + ny * Wd)
        out.append(f'<path d="M{p1[0]:.2f},{p1[1]:.2f} L{p2[0]:.2f},{p2[1]:.2f} '
                   f'L{p3[0]:.2f},{p3[1]:.2f} Z" fill="{CYAN}"/>')
    return "".join(out)


def vertical_text(x, y, s, size, fill, spacing):
    """COMPRESSION+GAIN running down the right edge, glyphs rotated 90 deg."""
    out = []
    for i, ch in enumerate(s):
        yy = y + i * spacing
        out.append(f'<g transform="translate({x:.2f},{yy:.2f}) rotate(90)">'
                   f'<text x="0" y="0" font-family="{F}" font-weight="bold" font-size="{size}" '
                   f'fill="{fill}" text-anchor="middle">{ch}</text></g>')
    return "".join(out)


def panel_svg(scale=1.0, include_meter=True, needle_at=None):
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{PANEL_W*scale:.0f}" '
         f'height="{PANEL_H*scale:.0f}" viewBox="0 0 {PANEL_W} {PANEL_H}">', defs()]
    s.append(f'<rect width="{PANEL_W}" height="{PANEL_H}" fill="url(#gPanel)"/>')

    # ---- header
    s.append(api_logo())
    s.append(T(175.5, 27.0, "525A", 21.5, CYAN, "end"))
    s.append(screw(*SCREW_TOP))

    # ---- THRESH / MAKE-UP
    for (cx, cy) in (THRESH, MAKEUP):
        s.append(dots(cx, cy, SMALL_DOT_R, SMALL_DOT_ANGLES))
    s.append(T(THRESH[0], 121.0, "IN", 12.4, WHITE))
    s.append(T(THRESH[0] + 1.0, 134.0, "THRESH", 14.5, CYAN))
    s.append(T(MAKEUP[0], 121.0, "OUT", 12.4, WHITE))
    s.append(T(MAKEUP[0] + 2.5, 134.0, "MAKE-UP", 14.5, CYAN))

    # ---- meter
    if include_meter:
        s.append(meter_face(with_needle_at=needle_at))

    # ---- GR led
    s.append(led(GR_LED[0], GR_LED[1], GR_LED_R, LED_R_OFF))
    s.append(T(163.0, 224.5, "GR", 13.0, WHITE, "start"))

    # ---- ATTACK
    ax, ay = ATTACK
    s.append(dots(ax, ay, SMALL_DOT_R, SMALL_DOT_ANGLES))
    # ORIGINAL hardware placement: every dot is a detent, and each label sits BESIDE its dot —
    # the extremes are 15µ (−150°) and 15 (+150°), nudged outward so they flank the ATTACK
    # caption instead of colliding with it; the unlabeled dots are stops too.
    lab = [("15&#181;", -150, 48.0, -18.0, -10.0), (".25", -120, 48.5, 0.0, 0.0),
           ("1", -90, 47.0, 0.0, 0.0), ("2", -60, 47.5, 0.0, 0.0),
           ("5", 60, 47.5, 0.0, 0.0), ("10", 120, 49.5, 0.0, 0.0),
           ("15", 150, 48.0, 18.0, -10.0)]
    for txt, ang, rr, dx, dy in lab:
        x, y = polar(ax, ay, rr, ang)
        s.append(T(x + dx, y + 3.8 + dy, txt, 11.0, WHITE))
    s.append(T(ax + 0.5, 317.5, "ATTACK", 15.5, WHITE))
    s.append(T(ax, 329.0, "mSEC", 10.0, CYAN))

    # ---- release ladder
    s.append(f'<path d="M{REL_LED_X},{REL_LEDS[0]} L{REL_LED_X},{REL_LEDS[-1]+8.0} '
             f'L{BTN_REL[0]+8.0},{BTN_REL[1]-10.0}" fill="none" stroke="{CYAN}" stroke-width="2.3" '
             f'stroke-linecap="round" stroke-linejoin="round"/>')
    for y in REL_LEDS:
        s.append(led(REL_LED_X, y, REL_LED_R, LED_G_OFF))
    for txt, y in (("2.0", 250.5), ("0.5", 275.0), ("0.2", 299.5), (".05", 324.0)):
        s.append(T(161.0, y + 4.0, txt, 12.0, WHITE, "start"))
    for txt, y in (("1.0", 263.0), ("0.3", 287.5), ("0.1", 312.0)):
        s.append(T(147.0, y + 4.2, txt, 12.5, CYAN, "end"))

    # ---- buttons legends
    s.append(T(33.0, 361.0, "C", 15.0, WHITE))
    s.append(T(32.0, 373.0, "2:1", 11.0, CYAN))
    s.append(T(165.5, 359.5, "REL", 15.0, WHITE))
    s.append(T(167.0, 372.0, "SEC", 10.5, CYAN))
    s.append(T(31.0, 417.5, "L", 15.0, WHITE))
    s.append(T(32.0, 429.0, "20:1", 11.0, CYAN))
    s.append(T(166.0, 417.0, "BYP", 15.0, WHITE))

    # ---- CEILING
    cx, cy = CEILING
    s.append(dots(cx, cy, CEIL_DOT_R, CEIL_DOT_ANGLES))
    for txt, ang in (("0", -150), ("4", -90), ("8", -30), ("12", 30), ("16", 90), ("20", 150)):
        x, y = polar(cx, cy, 71.5 if abs(ang) != 90 else 74.0, ang)
        s.append(T(x, y + 4.6, txt, 13.5, WHITE))
    s.append(T(81.0, 604.0, "CEILING", 14.0, WHITE))
    s.append(ceiling_arc())
    s.append(T(151.0, 462.0, "LESS", 11.0, CYAN))
    s.append(T(149.0, 616.5, "MORE", 11.0, CYAN))
    s.append(vertical_text(174.5, 463.0, "COMPRESSION+GAIN", 10.0, CYAN, 8.35))
    s.append(screw(*SCREW_BOT, lit=False))

    # ---- panel edge shading
    s.append(f'<rect width="{PANEL_W}" height="{PANEL_H}" fill="url(#gPanelX)"/>')
    s.append(f'<rect x="0" y="0" width="{PANEL_W}" height="0.9" fill="#ffffff" fill-opacity="0.30"/>')
    s.append(f'<rect x="0" y="{PANEL_H-1.0}" width="{PANEL_W}" height="1.0" fill="#000" fill-opacity="0.6"/>')
    s.append("</svg>")
    return "\n".join(s)


def panel_texture(img, seed=7, amount=7.0):
    a = np.array(img).astype(np.float32)
    rng = np.random.default_rng(seed)
    n = rng.normal(0, amount, a.shape[:2])
    # slight horizontal grain
    n = 0.65 * n + 0.35 * np.roll(n, 1, axis=1)
    a[:, :, :3] = np.clip(a[:, :, :3] + n[:, :, None], 0, 255)
    return Image.fromarray(a.astype(np.uint8), img.mode)
