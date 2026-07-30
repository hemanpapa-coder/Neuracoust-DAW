"""Rebuild ONLY panel_background.png at scales 1/2/3 (the knob strips are unchanged)."""
import os
from common import *
from knobs import render_svg
import panel as P

def px(v, s): return int(round(v * s))
OUT = os.path.join(os.path.dirname(__file__), "..", "assets")
for s in (1, 2, 3):
    img = render_svg(P.panel_svg(), px(PANEL_W, s), px(PANEL_H, s))
    img = P.panel_texture(img, amount=5.5 + s)
    fp = os.path.join(OUT, f"{s}x", "panel_background.png")
    img.save(fp)
    print("wrote", fp)
