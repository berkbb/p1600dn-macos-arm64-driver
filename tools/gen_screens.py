#!/usr/bin/env python3
"""Generate the driver's halftone screen tables (src/p1600_screen_tables.h).

The tables are the project's own design and hold no third-party data: the dot
shape, the dot positions, the growth order and the tone curves are defined
here. The few constants of the tone curves (TONE_1BPP, TONE_2BPP) were chosen
during development so that grays print at the intended darkness on this
printer.

Usage: tools/gen_screens.py <output.h>

Design
------
Both modes use a 45 degree clustered-dot screen:

* 600 dpi, 1 bit/pixel: dot lattice (4,4) and (4,-4), 8 dots per 16x16 tile.
* FastRes 1200, 2 bits/pixel: dot lattice (3,3) and (3,-3), 8 dots per 12x12
  tile, 4 toner levels per pixel.

Inside one dot cell the pixels are ranked by a spot function written in the
cell's own (rotated) axes u, v in [-1, 1): key = dot / (dot + hole), where dot
is the elliptical distance (axis ratio q_dot) from the dot center and hole the
one (axis ratio q_hole) from the nearest cell corner, the center of the white
hole. With both ratios 1 (600 dpi) the dot is round, grows into a cross and
meets its neighbours in a checkerboard at 50%; with q_dot < 1 (FastRes) the
dot is elliptical and first joins its neighbours along the long (u) axis, so
the midtones show chains. A pixel may only join a dot it touches, which keeps
every dot one solid piece. Keys are exact fractions, so ties never depend on
rounding.

All 8 cells of a tile are translates of each other and share this ranking.
The cells are interleaved in a dispersed order (each next cell is the one
farthest from the cells already chosen), so a gray level is spread evenly
over the 8 dots and no dot is more than one pixel ahead of another.

FastRes (2 bits) adds two things. Past ROUND_HOLES_2BPP its white pixels fill
by their round distance from the hole center, so the dark tones show round
holes in a solid net instead of chains. And its dot edges are soft: a pixel
reaches level 2 and then level 3 only some growth ranks after the previous
level (SPREAD_2BPP; SPREAD_DARK_2BPP in the round-hole phase), so each dot
carries a rim of partial pixels (levels 1 and 2) around a solid core, which
draws rounder, smoother dots.

The growth order is then mapped onto the tone curve (TONE_*): coverage(d), a
Bernstein polynomial in d / 255 from 0 to 1, is evaluated exactly, and the
k-th unit of toner switches on at the first darkness whose rounded coverage
reaches k+1.
"""
import math
import sys
from fractions import Fraction

# Tone curves: coverage (fraction of maximum toner) as a function of
# t = darkness / 255, each a Bernstein polynomial with end values 0 and 1 and
# these inner control values. Chosen during development so that grays print
# at the intended darkness on this printer (coverage within 1.9% of the target
# at 600 dpi and 1.6% in FastRes).
TONE_1BPP = [Fraction("0.15"), Fraction("0.363"), Fraction("0.156"), Fraction("0.885")]
TONE_2BPP = [Fraction("0.355"), Fraction("0.181")]

# 600 dpi: round dot (axis ratios 1) centered on a pixel corner, so a dot
# starts as a 2x2 square and grows into a cross.
PHASE_1BPP = (Fraction(1, 2), Fraction(1, 2))
Q_DOT_1BPP = Fraction(1)
Q_HOLE_1BPP = Fraction(1)

# FastRes: elliptical dot centered on a pixel, round holes.
PHASE_2BPP = (Fraction(0), Fraction(0))
Q_DOT_2BPP = Fraction(4, 5)  # short/long axis ratio: chains in the midtones
Q_HOLE_2BPP = Fraction(1)
# Pixels whose key is past ROUND_HOLES_2BPP fill by their round distance from
# the hole center instead, so the dark tones show round holes.
ROUND_HOLES_2BPP = Fraction(3, 10)
# Growth ranks from a pixel's level 1 to its level 2, and from level 2 to
# level 3 (0, 0 = one pixel at a time, as at 600 dpi; larger = a wider rim of
# partial pixels).
SPREAD_2BPP = (Fraction(3, 4), Fraction(2))
# The same for the pixels of the round-hole phase, narrower so the solid net
# closes right behind the shrinking holes.
SPREAD_DARK_2BPP = (Fraction(1, 2), Fraction(1))


# --------------------------------------------------------------------------
# Tone curve
# --------------------------------------------------------------------------
def bernstein(inner, t):
    """Bernstein polynomial with control values 0, *inner, 1 at t (exact)."""
    ys = [Fraction(0)] + list(inner) + [Fraction(1)]
    n = len(ys) - 1
    return sum(math.comb(n, k) * t ** k * (1 - t) ** (n - k) * ys[k] for k in range(n + 1))


def unit_thresholds(tone, units):
    """Darkness at which each of `units` toner units (in growth order) turns on.

    Unit k turns on at the first darkness d >= 1 whose target count
    coverage(d) * units, rounded half up, reaches k + 1; the last unit lands
    on 255. All arithmetic is exact.
    """
    count = [0] * 256
    for d in range(1, 256):
        target = math.floor(bernstein(tone, Fraction(d, 255)) * units + Fraction(1, 2))
        count[d] = max(count[d - 1], min(units, target))
    count[255] = units
    out, d = [], 1
    for k in range(units):
        while count[d] < k + 1:
            d += 1
        out.append(d)
    return out


# --------------------------------------------------------------------------
# Screen geometry
# --------------------------------------------------------------------------
def cell_coords(x, y, period, phase):
    """Map pixel (x, y) to (cell index (i, j), local u, v in [-1, 1)).

    Dot centers sit at phase + i*(p, p) + j*(p, -p).
    a = (dx + dy) / 2 and b = (dx - dy) / 2 run along the two 45 degree axes;
    a lattice step changes a or b by `period`.  Half-open cells give every
    dot exactly the same set of pixels.
    """
    x, y = x - phase[0], y - phase[1]
    a, b = Fraction(x + y, 2), Fraction(x - y, 2)
    i = math.floor(a / period + Fraction(1, 2))
    j = math.floor(b / period + Fraction(1, 2))
    u = (a - i * period) / Fraction(period, 2)
    v = (b - j * period) / Fraction(period, 2)
    return (i, j), u, v


def spot_key(u, v, q_dot, q_hole, round_holes=None):
    """Growth priority of a cell offset (smaller fills first).

    dot  = elliptical distance from the dot center (axis ratio q_dot, long
           axis u),
    hole = elliptical distance from the nearest cell corner, i.e. the center
           of the white hole between four dots (axis ratio q_hole).
    The key dot / (dot + hole) runs from 0 at the dot center to 1 at the hole
    center.  The contact point with the neighbour along u, (1, 0), gets
    q_hole^2 / (q_hole^2 + 1) and the one along v, (0, 1), gets
    1 / (1 + q_dot^2): with both ratios 1 the dots meet in a checkerboard at
    1/2; with q_dot < 1 they join along u first (chains).
    With round_holes, the pixels whose key is past it come after all others
    and fill from the farthest to the nearest to the hole center by round
    distance, so the last white pixels form round holes.
    Keys and distances are exact fractions; the angle only orders exact ties.
    """
    au, av = abs(u), abs(v)
    dot = au * au + (av / q_dot) ** 2
    hole = (1 - au) ** 2 + ((1 - av) / q_hole) ** 2
    key = dot / (dot + hole)
    if round_holes is not None and key > round_holes:
        return (1, -((1 - au) ** 2 + (1 - av) ** 2), key, math.atan2(v, u))
    # Tie-break: closer to the center first, then a fixed angular sweep.
    return (0, key, u * u + v * v, math.atan2(v, u))


def dispersed_cell_order(cells, tile, period):
    """Order cells so each next one is farthest from those already chosen."""
    def center(c):
        i, j = c
        return (period * (i + j)) % tile, (period * (i - j)) % tile
    def dist2(c1, c2):
        (x1, y1), (x2, y2) = center(c1), center(c2)
        dx, dy = abs(x1 - x2), abs(y1 - y2)
        dx, dy = min(dx, tile - dx), min(dy, tile - dy)
        return dx * dx + dy * dy
    order = [min(cells)]
    while len(order) < len(cells):
        rest = [c for c in sorted(cells) if c not in order]
        order.append(max(rest, key=lambda c: min(dist2(c, o) for o in order)))
    return order


def growth_order(tile, period, phase, q_dot, q_hole, round_holes=None):
    """Return the tile pixels (x, y) in screen growth order."""
    offset_of, cells = {}, {}
    for y in range(tile):
        for x in range(tile):
            (i, j), u, v = cell_coords(x, y, period, phase)
            cid = canonical_cell(i, j)
            offset_of[x, y] = (u, v)
            cells.setdefault(cid, {})[(u, v)] = (x, y)
    offsets = sorted(next(iter(cells.values())))
    assert len(cells) == 8 and all(sorted(c) == offsets for c in cells.values()), \
        "cells must be equal translates"

    # Rank the offsets of one cell.  Every dot has the same shape, so a pixel
    # is "next to the dot" when a 4-neighbour (in any cell) is already black.
    # Growing only along such pixels keeps each dot one solid piece and stops
    # a lone contact pixel from appearing between two dots before they reach it.
    black, ranked = set(), []
    while len(ranked) < len(offsets):
        def touches(off):
            x, y = cells[0, 0][off]
            return any(offset_of[(x + dx) % tile, (y + dy) % tile] in black
                       for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        free = [o for o in offsets if o not in black]
        near = [o for o in free if touches(o)] or free
        best = min(near, key=lambda o: spot_key(*o, q_dot, q_hole, round_holes))
        black.add(best)
        ranked.append(best)

    cell_order = dispersed_cell_order(list(cells), tile, period)
    order = []
    for rank, off in enumerate(ranked):
        # Rotate the dispersed order by a step coprime to 8 at every rank, so
        # no dot is systematically the one that is a pixel ahead.
        shift = (3 * rank) % 8
        for cid in cell_order[shift:] + cell_order[:shift]:
            order.append(cells[cid][off])
    return order


def canonical_cell(i, j):
    """Reduce lattice index (i, j) modulo the tile periods (2,2) and (2,-2)."""
    # (2,2) + (2,-2) = (4,0): reduce j to {0, 1} with (2,2) steps, then i mod 4.
    k = j // 2
    i, j = i - 2 * k, j - 2 * k
    return (i % 4, j)


# --------------------------------------------------------------------------
# Tables
# --------------------------------------------------------------------------
def build_1bpp():
    order = growth_order(16, 4, PHASE_1BPP, Q_DOT_1BPP, Q_HOLE_1BPP)
    thr = unit_thresholds(TONE_1BPP, len(order))
    table = [0] * 256
    for (x, y), t in zip(order, thr):
        table[y * 16 + x] = t
    return table


def build_2bpp():
    order = growth_order(12, 3, PHASE_2BPP, Q_DOT_2BPP, Q_HOLE_2BPP, ROUND_HOLES_2BPP)
    # A growth rank is one pixel in each of the 8 dots. Level 2 of the pixel
    # at position i comes SPREAD_2BPP[0] ranks after its level 1 and level 3
    # SPREAD_2BPP[1] ranks after that, so the levels of neighbouring rim
    # pixels interleave; ties go to the lower level, then to the earlier
    # pixel. Each pixel's levels stay in order.
    def offsets(x, y):
        _, u, v = cell_coords(x, y, 3, PHASE_2BPP)
        late = spot_key(u, v, Q_DOT_2BPP, Q_HOLE_2BPP, ROUND_HOLES_2BPP)[0] == 1
        s1, s2 = SPREAD_DARK_2BPP if late else SPREAD_2BPP
        return (0, s1 * 8, (s1 + s2) * 8)
    units = sorted((i + offsets(*order[i])[lev], lev, i)
                   for i in range(len(order)) for lev in range(3))
    thr = unit_thresholds(TONE_2BPP, len(units))
    # first[y][x][L-1]: darkness at which pixel (x, y) reaches level L.
    first = [[[256] * 3 for _ in range(12)] for _ in range(12)]
    for (_, lev, i), t in zip(units, thr):
        x, y = order[i]
        first[y][x][lev] = t
    table = [0] * (256 * 48)
    for d in range(256):
        for y in range(12):
            for x in range(12):
                lev = sum(1 for t in first[y][x] if d >= t)
                table[d * 48 + y * 4 + x // 4] |= lev << (6 - 2 * (x % 4))
    return table


def c_array(name, values, per_line=16):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join("%3d" % v for v in values[i:i + per_line]))
    return "static const unsigned char %s[%d] = {\n%s\n};\n" % (
        name, len(values), ",\n".join(lines))


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: gen_screens.py <output.h>")
    t1, t2 = build_1bpp(), build_2bpp()
    text = (
        "/* Halftone screen tables, generated by tools/gen_screens.py. Do not edit.\n"
        " * 600 dpi, 1 bit: 45 degree round dot, 8 dots per 16x16 tile,\n"
        " *   p1600_screen_1bpp[row*16 + x] is the darkness that turns the pixel black.\n"
        " * FastRes 1200, 2 bits: 45 degree elliptical dot with soft edges and round\n"
        " *   holes in the dark tones, 8 dots per 12x12 tile,\n"
        " *   p1600_screen_2bpp[d*48 + row*4 + x/4] holds four 2-bit levels.\n"
        " * The project's own design. */\n"
        "#ifndef P1600_SCREEN_TABLES_H\n#define P1600_SCREEN_TABLES_H\n\n"
        + c_array("p1600_screen_1bpp", t1) + "\n"
        + c_array("p1600_screen_2bpp", t2) + "\n#endif /* P1600_SCREEN_TABLES_H */\n")
    with open(sys.argv[1], "w") as fh:
        fh.write(text)


if __name__ == "__main__":
    main()
