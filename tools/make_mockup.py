#!/usr/bin/env python3
"""
Render the radar UI at the device's own resolution.

Draws what the glass shows: 480x480, the palette and geometry from
src/ui/radar.cpp, then scales up with nearest-neighbour so the pixel grid stays
visible. It is a mockup of the interface, not a photograph of a device.

    python3 tools/make_mockup.py        (needs Pillow)
"""
import math
import os

from PIL import Image, ImageDraw, ImageFont

SIZE = 480                 # the panel's real resolution
SCALE = 2                  # upscale factor, nearest-neighbour
CENTRE = SIZE // 2
PLOT_RADIUS = 196          # outermost range ring, as in the firmware
RADIUS_NM = 25.0

# Palette from src/ui/radar.cpp
BG          = (0x04, 0x07, 0x0D)
RING        = (0x14, 0x35, 0x2A)
RING_MAJOR  = (0x1E, 0x4D, 0x3B)
RING_LABEL  = (0x3E, 0x5C, 0x50)
NORTH       = (0xE8, 0xF0, 0xA0)
CARDINAL    = (0x5C, 0x7A, 0x6E)
HOME        = (0x38, 0xE0, 0x8A)
LABEL       = (0xAF, 0xC4, 0xD4)
DIM         = (0x6E, 0x8A, 0xA6)
SELECTED    = (0xFF, 0xFF, 0xFF)
EMERGENCY   = (0xFF, 0x3B, 0x30)
PANEL       = (0x0C, 0x12, 0x1C)
PANEL_EDGE  = (0x24, 0x34, 0x4A)

ALT_LOW  = (0x46, 0xD3, 0xFF)
ALT_MID  = (0x38, 0xE0, 0x8A)
ALT_HIGH = (0xD8, 0xD2, 0x4A)
ALT_TOP  = (0xEF, 0xF3, 0xF8)

FONT_DIR = "/System/Library/Fonts/Supplemental"
def font(size, bold=False):
    name = "Arial Bold.ttf" if bold else "Arial.ttf"
    path = os.path.join(FONT_DIR, name)
    if os.path.exists(path):
        return ImageFont.truetype(path, size)
    return ImageFont.load_default()

# bearing, distance_nm, track, colour, callsign, trail points
FLIGHTS = [
    (28,   8.0, 205, ALT_MID,  "BEL4OZ", 7),
    (95,  15.0, 268, ALT_HIGH, "KLM82R", 8),
    (152, 19.5, 340, ALT_TOP,  "DLH9HT", 8),
    (198, 10.5,  25, ALT_LOW,  "TRA61K", 6),
    (262, 15.0,  88, ALT_TOP,  "AFR23W", 9),
    (312, 13.5, 130, ALT_MID,  "EJU84N", 7),
    (340, 22.5, 165, ALT_HIGH, None,     8),
    (68,  23.0, 250, ALT_TOP,  None,     7),
    (232, 21.0,  75, EMERGENCY,"AMC311", 7),
]
SELECTED_FLIGHT = 0        # index into FLIGHTS for the detail view

def polar(bearing_deg, distance_nm):
    a = math.radians(bearing_deg)
    r = (distance_nm / RADIUS_NM) * PLOT_RADIUS
    return CENTRE + r * math.sin(a), CENTRE - r * math.cos(a)

def blend(colour, factor):
    return tuple(int(c * factor) for c in colour)

def draw_face(draw):
    """Rings, ticks, cardinals, home dot - the grid sprite."""
    for step in (1, 2, 3, 4):
        r = PLOT_RADIUS * step / 4
        colour = RING_MAJOR if step == 4 else RING
        draw.ellipse([CENTRE-r, CENTRE-r, CENTRE+r, CENTRE+r], outline=colour,
                     width=2 if step == 4 else 1)
        d = r * 0.7071
        draw.text((CENTRE + d, CENTRE - d), str(int(RADIUS_NM * step / 4)),
                  font=font(12), fill=RING_LABEL, anchor="mm")

    for degrees in range(0, 360, 30):
        a = math.radians(degrees)
        draw.line([CENTRE + math.sin(a)*(PLOT_RADIUS-8), CENTRE - math.cos(a)*(PLOT_RADIUS-8),
                   CENTRE + math.sin(a)*PLOT_RADIUS, CENTRE - math.cos(a)*PLOT_RADIUS],
                  fill=RING, width=1)

    for name, degrees in (("N", 0), ("E", 90), ("S", 180), ("W", 270)):
        a = math.radians(degrees)
        x = CENTRE + math.sin(a) * (PLOT_RADIUS + 22)
        y = CENTRE - math.cos(a) * (PLOT_RADIUS + 22)
        draw.text((x, y), name, font=font(20, bold=True),
                  fill=NORTH if name == "N" else CARDINAL, anchor="mm")

    draw.ellipse([CENTRE-4, CENTRE-4, CENTRE+4, CENTRE+4], fill=HOME)

def draw_aircraft(draw, selected=None, labels=8):
    for index, (bearing, distance, track, colour, callsign, trail) in enumerate(FLIGHTS):
        x, y = polar(bearing, distance)
        is_selected = (index == selected)

        # Trail behind, fading with age
        back = math.radians(track + 180)
        for k in range(trail, 0, -1):
            x1 = x + math.sin(back) * k * 7.0
            y1 = y - math.cos(back) * k * 7.0
            x2 = x + math.sin(back) * (k - 1) * 7.0
            y2 = y - math.cos(back) * (k - 1) * 7.0
            draw.line([x1, y1, x2, y2], fill=blend(colour, 0.18 + 0.55 * (1 - k / trail)),
                      width=2)

        # Chevron along the track
        a = math.radians(track)
        ca, sa = math.cos(a), math.sin(a)
        def rot(px, py):
            return (x + (px * ca + py * sa), y + (px * sa - py * ca))
        draw.polygon([rot(0, 9), rot(-6, -6), rot(6, -6)], fill=colour)

        emergency = colour == EMERGENCY
        if emergency:
            # Impossible to miss: the double ring the firmware draws for
            # 7500/7600/7700, which no filter can hide.
            draw.ellipse([x-22, y-22, x+22, y+22], outline=EMERGENCY, width=1)
            draw.ellipse([x-23, y-23, x+23, y+23], outline=EMERGENCY, width=1)
        if is_selected:
            draw.ellipse([x-18, y-18, x+18, y+18], outline=SELECTED, width=2)

        if callsign and (index < labels or is_selected or emergency):
            if emergency:
                # Clear of the 23 px alert ring.
                draw.text((x + 28, y - 7), callsign, font=font(13), fill=EMERGENCY, anchor="lm")
                draw.text((x + 28, y + 8), "SQ 7700", font=font(13), fill=EMERGENCY, anchor="lm")
            else:
                draw.text((x + 13, y), callsign, font=font(13),
                          fill=SELECTED if is_selected else LABEL, anchor="lm")

def panel_half_width(y):
    dy = y - CENTRE
    squared = CENTRE * CENTRE - dy * dy
    return 0 if squared <= 0 else math.sqrt(squared)

def draw_detail(draw):
    """The panel that opens on a tap: a segment of the circle, edge to edge."""
    top = 288
    for y in range(top, SIZE):
        half = panel_half_width(y)
        if half <= 0:
            continue
        draw.line([CENTRE - half, y, CENTRE + half, y], fill=PANEL)
    half = panel_half_width(top)
    draw.line([CENTRE - half, top, CENTRE + half, top], fill=PANEL_EDGE)

    draw.text((CENTRE, top + 22), "BEL4OZ", font=font(21, bold=True), fill=SELECTED, anchor="mm")
    draw.text((CENTRE, top + 50), "BRU  >  EDI", font=font(14), fill=HOME, anchor="mm")
    draw.text((CENTRE, top + 72), "Brussels - Edinburgh", font=font(13), fill=DIM, anchor="mm")
    draw.text((CENTRE, top + 96), "Airbus A320-214  OO-SNB", font=font(13), fill=LABEL, anchor="mm")
    draw.text((CENTRE, top + 120), "FL067 climbing   334 kt   12.4 nm   291",
              font=font(13), fill=DIM, anchor="mm")

def render(path, with_detail=False):
    image = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    # The panel is round: everything lives inside the glass, corners stay clear.
    draw.ellipse([0, 0, SIZE-1, SIZE-1], fill=BG)
    draw_face(draw)
    draw_aircraft(draw, selected=SELECTED_FLIGHT if with_detail else None,
                  labels=0 if with_detail else 8)
    if with_detail:
        draw_detail(draw)

    # Mask the corners, so the round screen reads as round.
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, SIZE-1, SIZE-1], fill=255)
    image.putalpha(mask)

    # Nearest-neighbour: keep the device's pixel grid visible rather than
    # inventing detail that the panel does not have.
    image = image.resize((SIZE*SCALE, SIZE*SCALE), Image.NEAREST)
    image.save(path)
    print(f"{path}: {image.width}x{image.height}")

os.makedirs("docs", exist_ok=True)
render("docs/ui-radar.png")
render("docs/ui-detail.png", with_detail=True)
