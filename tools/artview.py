#!/usr/bin/env python3
"""artview — a kitty-graphics image gallery, and a demonstration of what #83 costs.

Two jobs, deliberately in one tool:

  1. Show what the terminal can actually do. Kitty renders a full-resolution
     image scaled into a cell box, so a 1280x720 painting looks like a
     painting. This is the reference: the ceiling TermForge is aiming at.

  2. Show what TermForge can do *today*. `KittyDriver::draw_image` passes an
     Image's PIXEL dimensions as the placement's CELL dimensions
     (kitty_driver.cpp, `place_classic(slot, x, y, img->width(), img->height())`),
     so one image pixel occupies one terminal cell. An image that fits the
     screen is therefore about 100x40 pixels. Press `t` to cycle tiers and see
     the same artwork at that ceiling, next to the real one.

That second view is the argument for issue #83 (cell-rect draw_image +
preferred_pixel_extent) in a form no design doc conveys.

Stdlib only, except ImageMagick's `convert` for the downscaled tiers — this is
a dev tool, not library code, and TermForge's stdlib-only rule is about what
ships in the library.

Usage:
    tools/artview.py [DIR]        # default: assets/art

Keys:
    n / right / space   next image        p / left   previous
    t                   cycle tier: kitty full-res -> half-block -> 1px/cell
    r                   redraw            q / Esc    quit
"""

import base64
import os
import select
import shutil
import subprocess
import sys
import termios
import tty

CSI = "\033["
APC = "\033_G"
ST = "\033\\"

TIERS = [
    ("kitty, full resolution", "what the terminal can do"),
    ("half-block cells (AnsiRgbDriver tier)", "2 pixels per cell, no graphics protocol"),
    ("1 pixel per cell (TermForge today)", "the #83 ceiling: KittyDriver's cell box IS the pixel count"),
]


def term_size():
    sz = shutil.get_terminal_size((80, 24))
    return sz.columns, sz.lines


def cell_pixels(fd):
    """Ask the terminal for its cell size in pixels (CSI 16 t -> CSI 6;h;w t).

    Falls back to a common 8x17 if the terminal stays quiet, which only costs
    aspect accuracy. Never blocks longer than the timeout: a terminal that
    does not implement the query is the normal case, not an error.
    """
    sys.stdout.write(f"{CSI}16t")
    sys.stdout.flush()
    reply = ""
    while True:
        r, _, _ = select.select([fd], [], [], 0.15)
        if not r:
            break
        reply += os.read(fd, 64).decode("latin1")
        if "t" in reply:
            break
    try:
        body = reply.split("[")[1].rstrip("t")
        parts = body.split(";")
        if parts[0] == "6":
            return int(parts[2]), int(parts[1])
    except (IndexError, ValueError):
        pass
    return 8, 17


def fit_cells(iw, ih, cw, ch, max_cols, max_rows):
    """Largest cell box preserving the image's aspect ratio.

    Cells are taller than they are wide, so fitting by cell count alone
    stretches the picture — the arithmetic has to happen in pixels and convert
    back, which is exactly the calculation `preferred_pixel_extent()` would
    let a widget do (#83/#100).
    """
    box_w, box_h = max_cols * cw, max_rows * ch
    scale = min(box_w / iw, box_h / ih)
    cols = max(1, int(iw * scale / cw))
    rows = max(1, int(ih * scale / ch))
    return min(cols, max_cols), min(rows, max_rows)


def kitty_clear():
    sys.stdout.write(f"{APC}a=d,d=A{ST}")


def kitty_show(path, col, row, cols, rows):
    """Transmit a PNG and place it in a cols x rows cell box (kitty scales it).

    f=100 hands kitty the PNG bytes directly — no decoding here. The escape
    payload is chunked at 4096 base64 bytes with m=1 continuation, which the
    protocol requires and which TermForge's own transmit path does too.
    """
    with open(path, "rb") as fh:
        payload = base64.standard_b64encode(fh.read()).decode("ascii")
    sys.stdout.write(f"{CSI}{row};{col}H")
    first, chunks = True, [payload[i:i + 4096] for i in range(0, len(payload), 4096)]
    for i, chunk in enumerate(chunks):
        more = 1 if i < len(chunks) - 1 else 0
        if first:
            sys.stdout.write(
                f"{APC}a=T,f=100,t=d,c={cols},r={rows},C=1,q=2,m={more};{chunk}{ST}")
            first = False
        else:
            sys.stdout.write(f"{APC}m={more};{chunk}{ST}")
    sys.stdout.flush()


def rgb_pixels(path, w, h):
    """Downscale to w x h and return raw RGB bytes, via ImageMagick."""
    out = subprocess.run(
        ["convert", path, "-resize", f"{w}x{h}!", "-depth", "8", "rgb:-"],
        capture_output=True, check=True).stdout
    return out


def draw_halfblock(path, col, row, cols, rows):
    """Two pixels per cell with the upper-half-block glyph — the AnsiRgbDriver tier.

    Foreground paints the top pixel, background the bottom. This is the
    universal floor TermForge falls back to when there is no graphics
    protocol, and on a photograph it holds up better than people expect.
    """
    px = rgb_pixels(path, cols, rows * 2)
    stride = cols * 3
    out = []
    for cy in range(rows):
        out.append(f"{CSI}{row + cy};{col}H")
        for cx in range(cols):
            t = (cy * 2) * stride + cx * 3
            b = (cy * 2 + 1) * stride + cx * 3
            out.append(f"{CSI}38;2;{px[t]};{px[t+1]};{px[t+2]}m"
                       f"{CSI}48;2;{px[b]};{px[b+1]};{px[b+2]}m▀")
        out.append(f"{CSI}0m")
    sys.stdout.write("".join(out))
    sys.stdout.flush()


def draw_pixel_per_cell(path, col, row, cols, rows):
    """One pixel per cell — what TermForge's pixel path can express today.

    Not a strawman: this is precisely what KittyDriver produces, because the
    placement's c=/r= are handed the image's pixel dimensions. Painted here as
    coloured spaces so the resolution is the only variable.
    """
    px = rgb_pixels(path, cols, rows)
    out = []
    for cy in range(rows):
        out.append(f"{CSI}{row + cy};{col}H")
        for cx in range(cols):
            i = cy * cols * 3 + cx * 3
            out.append(f"{CSI}48;2;{px[i]};{px[i+1]};{px[i+2]}m ")
        out.append(f"{CSI}0m")
    sys.stdout.write("".join(out))
    sys.stdout.flush()


def image_size(path):
    out = subprocess.run(["identify", "-format", "%w %h", path],
                         capture_output=True, check=True).stdout.decode()
    w, h = out.split()
    return int(w), int(h)


def render(path, tier, cw, ch):
    cols, rows = term_size()
    kitty_clear()
    sys.stdout.write(f"{CSI}2J{CSI}H")

    iw, ih = image_size(path)
    art_rows = rows - 3
    bw, bh = fit_cells(iw, ih, cw, ch, cols, art_rows)
    left = max(1, (cols - bw) // 2 + 1)

    if tier == 0:
        kitty_show(path, left, 2, bw, bh)
    elif tier == 1:
        draw_halfblock(path, left, 2, bw, bh)
    else:
        draw_pixel_per_cell(path, left, 2, bw, bh)

    name, note = TIERS[tier]
    label = f" {os.path.basename(path)}  {iw}x{ih}  |  {name} — {note}"
    keys = " n/p image   t tier   r redraw   q quit"
    sys.stdout.write(f"{CSI}{rows-1};1H{CSI}2K{CSI}1m{label[:cols]}{CSI}0m")
    sys.stdout.write(f"{CSI}{rows};1H{CSI}2K{CSI}2m{keys[:cols]}{CSI}0m")
    sys.stdout.flush()


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "assets/art"
    images = sorted(os.path.join(root, f) for f in os.listdir(root)
                    if f.lower().endswith((".png", ".jpg", ".jpeg")))
    if not images:
        sys.exit(f"no images in {root}")

    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    sys.stdout.write(f"{CSI}?1049h{CSI}?25l")
    try:
        tty.setraw(fd)
        cw, ch = cell_pixels(fd)
        idx, tier = 0, 0
        render(images[idx], tier, cw, ch)
        while True:
            key = os.read(fd, 8).decode("latin1", "replace")
            if key in ("q", "\033"):
                break
            if key in ("n", " ", f"{CSI}C"):
                idx = (idx + 1) % len(images)
            elif key in ("p", f"{CSI}D"):
                idx = (idx - 1) % len(images)
            elif key == "t":
                tier = (tier + 1) % len(TIERS)
            elif key != "r":
                continue
            render(images[idx], tier, cw, ch)
    finally:
        kitty_clear()
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        sys.stdout.write(f"{CSI}?25h{CSI}?1049l")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
