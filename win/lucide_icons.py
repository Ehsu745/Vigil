#!/usr/bin/env python3
"""lucide_icons.py: writes win/lucide.h, the Lucide icons (ISC License, lucide.dev) the Settings window draws, each
flattened into polylines on Lucide's 24-unit grid (curves and arcs cut into short straight pieces), so panel.c can
draw them with its antialiased line helper. Standard library only. Run it again after changing ICONS:

    python3 win/lucide_icons.py
"""
import math, os, re

ICONS = {
 "palette": "M12 22a1 1 0 0 1 0-20 10 9 0 0 1 10 9 5 5 0 0 1-5 5h-2.25a1.75 1.75 0 0 0-1.4 2.8l.3.4a1.75 1.75 0 0 1-1.4 2.8zM13 6.5a0.5 0.5 0 1 0 1 0a0.5 0.5 0 1 0 -1 0M17 10.5a0.5 0.5 0 1 0 1 0a0.5 0.5 0 1 0 -1 0M6 12.5a0.5 0.5 0 1 0 1 0a0.5 0.5 0 1 0 -1 0M8 7.5a0.5 0.5 0 1 0 1 0a0.5 0.5 0 1 0 -1 0",
 "message": "M7.9 20A9 9 0 1 0 4 16.1L2 22Z",
 "gauge": "m12 14 4-4M3.34 19a10 10 0 1 1 17.32 0",
 "activity": "M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2",
 "eye": "M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0M9 12a3 3 0 1 0 6 0a3 3 0 1 0 -6 0",
 "puzzle": "M15.39 4.39a1 1 0 0 0 1.68-.474 2.5 2.5 0 1 1 3.014 3.015 1 1 0 0 0-.474 1.68l1.683 1.682a2.414 2.414 0 0 1 0 3.414L19.61 15.39a1 1 0 0 1-1.68-.474 2.5 2.5 0 1 0-3.014 3.015 1 1 0 0 1 .474 1.68l-1.683 1.682a2.414 2.414 0 0 1-3.414 0L8.61 19.61a1 1 0 0 0-1.68.474 2.5 2.5 0 1 1-3.014-3.015 1 1 0 0 0 .474-1.68l-1.683-1.682a2.414 2.414 0 0 1 0-3.414L4.39 8.61a1 1 0 0 1 1.68.474 2.5 2.5 0 1 0 3.014-3.015 1 1 0 0 1-.474-1.68l1.683-1.682a2.414 2.414 0 0 1 3.414 0z",
 "blend": "M2 9a7 7 0 1 0 14 0a7 7 0 1 0 -14 0M8 15a7 7 0 1 0 14 0a7 7 0 1 0 -14 0",
 "rotateCw": "M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8M21 3v5h-5",
 "rotateCcw": "M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8M3 3v5h5",
 "refresh": "M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8M21 3v5h-5M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16M8 16H3v5",
 "swords": "M14.5 17.5L3 6L3 3L6 3L17.5 14.5M13 19L19 13M16 16L20 20M19 21L21 19M14.5 6.5L18 3L21 3L21 6L17.5 9.5M5 14L9 18M7 17L4 20M3 19L5 21",
 "cog": "M12 20a8 8 0 1 0 0-16 8 8 0 0 0 0 16ZM12 14a2 2 0 1 0 0-4 2 2 0 0 0 0 4ZM12 2v2M12 22v-2m5-1.34-1-1.73M11 10.27 7 3.34m13.66 13.66-1.73-1M3.34 7l1.73 1M14 12h8M2 12h2m16.66-5-1.73 1M3.34 17l1.73-1M17 3.34l-1 1.73M11 13.73l-4 6.93",
 "orbit": "M9 12a3 3 0 1 0 6 0a3 3 0 1 0 -6 0M17 5a2 2 0 1 0 4 0a2 2 0 1 0 -4 0M3 19a2 2 0 1 0 4 0a2 2 0 1 0 -4 0M10.4 21.9a10 10 0 0 0 9.941-15.416M13.5 2.1a10 10 0 0 0-9.841 15.416",
 "dna": "m10 16 1.5 1.5M14 8l-1.5-1.5M15 2c-1.798 1.998-2.518 3.995-2.807 5.993m4.307 2.507 1 1M17 6l-2.891-2.891M2 15c6.667-6 13.333 0 20-6m-2 0 .891.891M3.109 14.109 4 15m2.5-2.5 1 1M7 18l2.891 2.891M9 22c1.798-1.998 2.518-3.995 2.807-5.993",
 "keyboard": "M4 4h16a2 2 0 0 1 2 2v12a2 2 0 0 1 -2 2h-16a2 2 0 0 1 -2 -2v-12a2 2 0 0 1 2 -2ZM6 8h.01M10 8h.01M14 8h.01M18 8h.01M8 12h.01M12 12h.01M16 12h.01M7 16h10",
 "hammer": "m15 12-8.373 8.373a1 1 0 1 1-3-3L12 9m6 6 4-4m-.5.5-1.914-1.914A2 2 0 0 1 19 8.172V7l-2.26-2.26a6 6 0 0 0-4.202-1.756L9 2.96l.92.82A6.18 6.18 0 0 1 12 8.4V10l2 2h1.172a2 2 0 0 1 1.414.586L18.5 14.5",
 "grid": "M5 3h14a2 2 0 0 1 2 2v14a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-14a2 2 0 0 1 2 -2ZM3 9h18M3 15h18M9 3v18M15 3v18",
 "arrows": "m16 3 4 4-4 4M20 7H4m4 14-4-4 4-4M4 17h16",
 "minimize": "M4 14L10 14L10 20M20 10L14 10L14 4M14 10L21 3M3 21L10 14",
 "fish": "M6.5 12c.94-3.46 4.94-6 8.5-6 3.56 0 6.06 2.54 7 6-.94 3.47-3.44 6-7 6s-7.56-2.53-8.5-6ZM18 12v.5M16 17.93a9.77 9.77 0 0 1 0-11.86M7 10.67C7 8 5.58 5.97 2.73 5.5c-1 1.5-1 5 .23 6.5-1.24 1.5-1.24 5-.23 6.5C5.58 18.03 7 16 7 13.33",
 "tower": "M4.9 16.1C1 12.2 1 5.8 4.9 1.9M7.8 4.7a6.14 6.14 0 0 0-.8 7.5M10 9a2 2 0 1 0 4 0a2 2 0 1 0 -4 0M16.9 4.7a6.14 6.14 0 0 1 .8 7.5M19.1 1.9a9.96 9.96 0 0 1 0 14.1M9.5 18h5m-6.5 4 4-11 4 11",
 "bell": "M10.268 21a2 2 0 0 0 3.464 0M22 8c0-2.3-.8-4.3-2-6M3.262 15.326A1 1 0 0 0 4 17h16a1 1 0 0 0 .74-1.673C19.41 13.956 18 12.499 18 8A6 6 0 0 0 6 8c0 4.499-1.411 5.956-2.738 7.326M4 2C2.8 3.7 2 5.7 2 8",
 "unlock": "M5 11h14a2 2 0 0 1 2 2v7a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-7a2 2 0 0 1 2 -2ZM7 11V7a5 5 0 0 1 9.9-1",
 "lock": "M5 11h14a2 2 0 0 1 2 2v7a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-7a2 2 0 0 1 2 -2ZM7 11V7a5 5 0 0 1 10 0v4",
 "flower": "M9 12a3 3 0 1 0 6 0a3 3 0 1 0 -6 0M12 16.5A4.5 4.5 0 1 1 7.5 12 4.5 4.5 0 1 1 12 7.5a4.5 4.5 0 1 1 4.5 4.5 4.5 4.5 0 1 1-4.5 4.5M12 7.5V9m-4.5 3H9m7.5 0H15m-3 4.5V15M8 8l1.88 1.88M14.12 9.88 16 8M8 16l1.88-1.88M14.12 14.12 16 16",
 "chevrons": "m7 6 5 5 5-5m-10 7 5 5 5-5",
 "moon": "M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z",
 "hole": "M2 12a10 10 0 1 0 20 0a10 10 0 1 0 -20 0M11 12a1 1 0 1 0 2 0a1 1 0 1 0 -2 0",
 "radio": "M4.9 19.1C1 15.2 1 8.8 4.9 4.9M7.8 16.2c-2.3-2.3-2.3-6.1 0-8.5M10 12a2 2 0 1 0 4 0a2 2 0 1 0 -4 0M16.2 7.8c2.3 2.3 2.3 6.1 0 8.5M19.1 4.9C23 8.8 23 15.1 19.1 19",
 "mic": "M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3ZM19 10v2a7 7 0 0 1-14 0v-2M12 19v3",
 "binary": "M16 14h0a2 2 0 0 1 2 2v2a2 2 0 0 1 -2 2h0a2 2 0 0 1 -2 -2v-2a2 2 0 0 1 2 -2ZM8 4h0a2 2 0 0 1 2 2v2a2 2 0 0 1 -2 2h0a2 2 0 0 1 -2 -2v-2a2 2 0 0 1 2 -2ZM6 20h4M14 10h4M6 14h2v6M14 4h2v6",
 "zap": "M4 14a1 1 0 0 1-.78-1.63l9.9-10.2a.5.5 0 0 1 .86.46l-1.92 6.02A1 1 0 0 0 13 10h7a1 1 0 0 1 .78 1.63l-9.9 10.2a.5.5 0 0 1-.86-.46l1.92-6.02A1 1 0 0 0 11 14z",
 "scanEye": "M3 7V5a2 2 0 0 1 2-2h2M17 3h2a2 2 0 0 1 2 2v2M21 17v2a2 2 0 0 1-2 2h-2M7 21H5a2 2 0 0 1-2-2v-2M11 12a1 1 0 1 0 2 0a1 1 0 1 0 -2 0M18.944 12.33a1 1 0 0 0 0-.66 7.5 7.5 0 0 0-13.888 0 1 1 0 0 0 0 .66 7.5 7.5 0 0 0 13.888 0",
 "vibrate": "m2 8 2 2-2 2 2 2-2 2m20-8-2 2 2 2-2 2 2 2M9 5h6a1 1 0 0 1 1 1v12a1 1 0 0 1 -1 1h-6a1 1 0 0 1 -1 -1v-12a1 1 0 0 1 1 -1Z",
 "sunDim": "M8 12a4 4 0 1 0 8 0a4 4 0 1 0 -8 0M12 4h.01M20 12h.01M12 20h.01M4 12h.01M17.657 6.343h.01M17.657 17.657h.01M6.343 17.657h.01M6.343 6.343h.01",
 "crosshair": "M2 12a10 10 0 1 0 20 0a10 10 0 1 0 -20 0M22 12L18 12M6 12L2 12M12 6L12 2M12 22L12 18",
 "step": "M6 4L6 20M10 4L20 12L10 20Z",
 "unfold": "M12 22v-6M12 8V2M4 12H2m8 0H8m8 0h-2m8 0h-2m-5 7-3 3-3-3m6-14-3-3-3 3",
 "scan": "M3 7V5a2 2 0 0 1 2-2h2M17 3h2a2 2 0 0 1 2 2v2M21 17v2a2 2 0 0 1-2 2h-2M7 21H5a2 2 0 0 1-2-2v-2M7 12h10",
 "triangle": "M13.73 4a2 2 0 0 0-3.46 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3Z",
 "alert": "m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3M12 9v4M12 17h.01",
 "waves": "M2 6c.6.5 1.2 1 2.5 1C7 7 7 5 9.5 5c2.6 0 2.4 2 5 2 2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1M2 12c.6.5 1.2 1 2.5 1 2.5 0 2.5-2 5-2 2.6 0 2.4 2 5 2 2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1M2 18c.6.5 1.2 1 2.5 1 2.5 0 2.5-2 5-2 2.6 0 2.4 2 5 2 2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1",
 "sparkles": "M9.937 15.5A2 2 0 0 0 8.5 14.063l-6.135-1.582a.5.5 0 0 1 0-.962L8.5 9.936A2 2 0 0 0 9.937 8.5l1.582-6.135a.5.5 0 0 1 .963 0L14.063 8.5A2 2 0 0 0 15.5 9.937l6.135 1.581a.5.5 0 0 1 0 .964L15.5 14.063a2 2 0 0 0-1.437 1.437l-1.582 6.135a.5.5 0 0 1-.963 0zM20 3v4M22 5h-4M4 17v2M5 18H3",
 "layers": "M12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83zM2 12a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 12M2 17a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 17",
 "eyeOff": "M10.733 5.076a10.744 10.744 0 0 1 11.205 6.575 1 1 0 0 1 0 .696 10.747 10.747 0 0 1-1.444 2.49M14.084 14.158a3 3 0 0 1-4.242-4.242M17.479 17.499a10.75 10.75 0 0 1-15.417-5.151 1 1 0 0 1 0-.696 10.75 10.75 0 0 1 4.446-5.143M2 2l20 20",
 "reply": "M20 18v-2a4 4 0 0 0-4-4H4M9 17l-5-5 5-5",
 "volume": "M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298zM16 9a5 5 0 0 1 0 6M19.364 18.364a9 9 0 0 0 0-12.728",
 "sliders": "M21 4h-7M10 4H3M21 12h-9M8 12H3M21 20h-5M12 20H3M14 2v4M8 10v4M16 18v4",
 "move": "M12 2v20m-3-17 3-3 3 3m-6 14 3 3 3-3M2 12h20M5 9l-3 3 3 3m14-6 3 3-3 3",
 "hand": "M18 11V6a2 2 0 0 0-2-2a2 2 0 0 0-2 2M14 10V4a2 2 0 0 0-2-2a2 2 0 0 0-2 2v2M10 10.5V6a2 2 0 0 0-2-2a2 2 0 0 0-2 2v8M18 8a2 2 0 1 1 4 0v6a8 8 0 0 1-8 8h-2c-2.8 0-4.5-.86-5.99-2.34l-3.6-3.6a2 2 0 0 1 2.83-2.82L7 15",
 "pointer": "M4.037 4.688a.495.495 0 0 1 .651-.651l16 6.5a.5.5 0 0 1-.063.947l-6.124 1.58a2 2 0 0 0-1.438 1.435l-1.579 6.126a.5.5 0 0 1-.947.063z",
 "send": "M14.536 21.686a.5.5 0 0 0 .937-.024l6.5-19a.496.496 0 0 0-.635-.635l-19 6.5a.5.5 0 0 0-.024.937l7.93 3.18a2 2 0 0 1 1.112 1.11zM21.854 2.147l-10.94 10.939",
 "newline": "M9 10l-5 5 5 5M20 4v7a4 4 0 0 1-4 4H4",
 "x": "M18 6 6 18M6 6l12 12",
 "panelLeft": "M3 5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2zM9 3v18M16 15l-3-3 3-3",
 "panelRight": "M3 5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2zM9 3v18M14 9l3 3-3 3",
 "minus": "M5 12h14",
 "plus": "M5 12h14M12 5v14"
}

def tokens(d):
    for m in re.finditer(r'[MmLlHhVvCcSsQqTtAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?', d):
        yield m.group(0)

def flatten(d):
    """the path's subpaths, each a list of (x, y)"""
    t = list(tokens(d)); i = 0; out = []; cur = None; x = y = sx = sy = 0.0; cmd = None; lc = None
    def num():
        nonlocal i
        v = float(t[i]); i += 1; return v
    def start(px, py):
        nonlocal cur
        cur = [(px, py)]; out.append(cur)
    def seg(px, py):
        if cur is None: start(x, y)
        cur.append((px, py))
    while i < len(t):
        if re.match(r'[A-Za-z]', t[i]): cmd = t[i]; i += 1
        rel = cmd.islower(); C = cmd.upper()
        if C == 'Z':
            if cur: cur.append(cur[0])
            x, y = sx, sy; cur = None; lc = None; continue
        if C == 'M':
            nx, ny = num(), num()
            if rel: nx += x; ny += y
            x, y = sx, sy = nx, ny; start(x, y); cmd = 'l' if rel else 'L'; lc = None; continue
        if C in 'LHV':
            if C == 'L': nx, ny = num(), num(); nx, ny = (nx + x, ny + y) if rel else (nx, ny)
            elif C == 'H': nx = num(); nx = nx + x if rel else nx; ny = y
            else: ny = num(); ny = ny + y if rel else ny; nx = x
            seg(nx, ny); x, y = nx, ny; lc = None; continue
        if C in 'CS':
            if C == 'C': x1, y1 = num(), num()
            else: x1, y1 = (2 * x - lc[0], 2 * y - lc[1]) if lc else (x, y)
            x2, y2, ex, ey = num(), num(), num(), num()
            if rel:
                if C == 'C': x1 += x; y1 += y
                x2 += x; y2 += y; ex += x; ey += y
            n = 10
            for k in range(1, n + 1):
                u = k / n; a = (1 - u) ** 3; b = 3 * u * (1 - u) ** 2; c = 3 * u * u * (1 - u); e = u ** 3
                seg(a * x + b * x1 + c * x2 + e * ex, a * y + b * y1 + c * y2 + e * ey)
            lc = (x2, y2); x, y = ex, ey; continue
        if C in 'QT':
            if C == 'Q': x1, y1 = num(), num(); x1, y1 = (x1 + x, y1 + y) if rel else (x1, y1)
            else: x1, y1 = (2 * x - lc[0], 2 * y - lc[1]) if lc else (x, y)
            ex, ey = num(), num(); ex, ey = (ex + x, ey + y) if rel else (ex, ey)
            for k in range(1, 9):
                u = k / 8; seg((1 - u) ** 2 * x + 2 * u * (1 - u) * x1 + u * u * ex, (1 - u) ** 2 * y + 2 * u * (1 - u) * y1 + u * u * ey)
            lc = (x1, y1); x, y = ex, ey; continue
        if C == 'A':
            rx, ry, rot, large, sweep, ex, ey = num(), num(), num(), num(), num(), num(), num()
            if rel: ex += x; ey += y
            for p in arc(x, y, rx, ry, rot, large, sweep, ex, ey): seg(*p)
            x, y = ex, ey; lc = None; continue
        raise ValueError('cannot read ' + cmd)
    return [p for p in out if len(p) > 1]

def arc(x1, y1, rx, ry, rot, large, sweep, x2, y2):
    """an SVG arc as points (the endpoint form turned into centre and angles)"""
    if rx == 0 or ry == 0: return [(x2, y2)]
    phi = math.radians(rot); cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    rx, ry = abs(rx), abs(ry)
    lam = x1p ** 2 / rx ** 2 + y1p ** 2 / ry ** 2
    if lam > 1: rx *= math.sqrt(lam); ry *= math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0, num / den)) if den else 0
    if large == sweep: co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx, cy = cp * cxp - sp * cyp + (x1 + x2) / 2, sp * cxp + cp * cyp + (y1 + y2) / 2
    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy); return a
    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0: dt -= 2 * math.pi
    elif sweep and dt < 0: dt += 2 * math.pi
    n = max(4, int(abs(dt) * max(rx, ry) * 1.2))
    pts = []
    for k in range(1, n + 1):
        t = t1 + dt * k / n
        pts.append((cx + rx * math.cos(t) * cp - ry * math.sin(t) * sp, cy + rx * math.cos(t) * sp + ry * math.sin(t) * cp))
    return pts

def main():
    names = list(ICONS); pts = []; runs = []; rows = []
    for name in names:
        subs = flatten(ICONS[name]); r0 = len(runs)
        for sub in subs:
            runs.append(len(sub)); pts.extend(sub)
        rows.append((name, r0, len(subs)))
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, 'lucide.h'), 'w', newline='\n') as f:
        f.write('/* lucide.h: written by lucide_icons.py, do not edit. Lucide icons (ISC License, lucide.dev) as polylines on\n'
                ' * their 24-unit grid: icon i is runs LU_ICON[i].run .. + n, each run that many points from LU_PTS in order. */\n')
        f.write('typedef struct { const char *name; unsigned short run, n; } LuIcon;\n')
        f.write('static const float LU_PTS[] = {\n')
        for k in range(0, len(pts), 8):
            f.write('    ' + ' '.join('%.2ff, %.2ff,' % p for p in pts[k:k + 8]) + '\n')
        f.write('};\nstatic const unsigned short LU_RUNS[] = {\n')
        for k in range(0, len(runs), 24):
            f.write('    ' + ', '.join(str(r) for r in runs[k:k + 24]) + ',\n')
        f.write('};\nstatic const LuIcon LU_ICON[] = {\n')
        for name, r0, n in rows: f.write('    {"%s", %d, %d},\n' % (name, r0, n))
        f.write('};\n#define LU_N ((int)(sizeof LU_ICON / sizeof LU_ICON[0]))\n')

if __name__ == '__main__':
    main()
