#!/usr/bin/env python3
"""
Convert an X11 bitmap font (PCF) into an LVGL 1-bpp C font.

    pcf_to_lvgl.py --pcf 6x13.pcf --size 13 --name fixed_6x13 -o out.c \
                   [--symbol-font FontAwesome.woff --symbols 0xF021,0xF071]

Why this exists: lv_font_conv is built on opentype.js and rejects PCF outright
("Unsupported OpenType signature fcp"). X11 misc-fixed has no TTF form, so the
only way to get 6x13 into LVGL is to emit the font table ourselves.

The output is byte-for-byte the same *shape* lv_font_conv produces for
--bpp 1 --no-compress: glyph rows packed MSB-first with no row padding, each
glyph starting on a byte boundary, one FORMAT0_TINY cmap for contiguous ASCII
and one SPARSE_TINY cmap for everything above it. Kerning is dropped - these
fonts are monospaced, so there is nothing to kern.

Glyphs are read through FreeType (via Pillow), which hands back a bitmap
strike verbatim: no hinting, no antialiasing, no scaling. The symbol glyphs
merged in from FontAwesome are outlines, so those alone get thresholded.
"""
import argparse
import sys

from PIL import Image, ImageDraw, ImageFont

# Left margin on the render canvas, so a glyph with negative bearing still
# lands inside the image and shows up in getbbox().
PAD = 8


class Glyph:
    def __init__(self, cp, adv_w, box_w, box_h, ofs_x, ofs_y, rows):
        self.cp = cp                # unicode codepoint
        self.adv_w = adv_w          # advance, 1/16 px (LVGL's kern_scale)
        self.box_w, self.box_h = box_w, box_h
        self.ofs_x, self.ofs_y = ofs_x, ofs_y
        self.rows = rows            # list of lists of 0/1, box_h x box_w
        self.index = 0              # filled in when the bitmap is laid out

    def packed(self):
        """Rows concatenated MSB-first, padded to a whole byte at the end."""
        out, acc, nbits = bytearray(), 0, 0
        for row in self.rows:
            for px in row:
                acc = (acc << 1) | px
                nbits += 1
                if nbits == 8:
                    out.append(acc)
                    acc, nbits = 0, 0
        if nbits:
            out.append(acc << (8 - nbits))
        return bytes(out)


def render(font, ch, ascent, descent, threshold):
    """Rasterise one character; return (box_w, box_h, ofs_x, ofs_y, rows).

    The canvas is padded on every side so a glyph that overhangs its advance -
    an outline icon scaled past its nominal size, say - is measured rather than
    clipped. Offsets are relative to the pen position and this font's own
    baseline, so a merged symbol font needs no knowledge of the text font.
    """
    w = PAD * 2 + int(font.getlength(ch) or 0) + PAD
    img = Image.new("L", (max(w, PAD * 2 + 1), ascent + descent + PAD * 2), 0)
    # Default anchor is "la": x is the pen position, y is the ascender line.
    ImageDraw.Draw(img).text((PAD, PAD), ch, font=font, fill=255)
    if threshold:
        img = img.point(lambda v: 255 if v >= 128 else 0)
    bbox = img.getbbox()
    if bbox is None:
        return 0, 0, 0, 0, []
    x0, y0, x1, y1 = bbox
    px = img.load()
    rows = [[1 if px[x, y] else 0 for x in range(x0, x1)] for y in range(y0, y1)]
    # ofs_y is the gap from the baseline up to the bottom edge of the box, so a
    # descender comes out negative.
    return x1 - x0, y1 - y0, x0 - PAD, (ascent + PAD) - y1, rows


def embolden_rows(rows):
    """
    Widen every stem by one pixel: OR each row with itself shifted right.

    misc-fixed ships bold cuts for 6x13, 7x14, 8x13 and 9x15 but not for 10x20,
    so the largest rung has to be thickened synthetically. The glyph box grows
    by a pixel while the advance does not, which keeps the monospaced grid the
    pages do their column arithmetic on - the extra pixel comes out of the
    right-hand side bearing.
    """
    out = []
    for row in rows:
        ext = row + [0]          # one more column for the shifted copy to land in
        out.append([ext[x] | (ext[x - 1] if x else 0) for x in range(len(ext))])
    return out


def build(font, codepoints, ascent, descent, threshold, adv_override=None,
          scale=1, bold=False):
    glyphs = []
    for cp in codepoints:
        ch = chr(cp)
        if font.getlength(ch) == 0 and cp != 0x20:
            continue
        bw, bh, ox, oy, rows = render(font, ch, ascent, descent, threshold)
        if bw == 0:
            # A blank glyph still needs a box, or LVGL reads a zero-size bitmap.
            bw, bh, ox, oy, rows = 1, 1, 0, 0, [[0]]
        adv = adv_override(bw) if adv_override else font.getlength(ch)
        if bold and rows:
            rows = embolden_rows(rows)
            bw += 1

        if scale > 1:
            # Integer pixel doubling. misc-fixed stops at 10x20, so the one
            # oversized alert on the pack page has to come from somewhere; a
            # doubled strike keeps the family and reads as deliberate, where
            # the 12x24 strike is a different (near-serif, JIS) design that
            # does not sit with the rest of the ladder at all.
            rows = [[px for px in r for _ in range(scale)] for r in rows
                    for _ in range(scale)]
            bw, bh, ox, oy = bw * scale, bh * scale, ox * scale, oy * scale
            adv *= scale
        glyphs.append(Glyph(cp, round(adv * 16), bw, bh, ox, oy, rows))
    return glyphs


def emit(path, name, glyphs, line_height, base_line, argv):
    for g in glyphs:
        g.blob = g.packed()
    idx = 0
    for g in glyphs:
        g.index = idx
        idx += len(g.blob)

    # lv_font_fmt_txt_glyph_dsc_t packs these into bitfields when
    # LV_FONT_FMT_TXT_LARGE is 0, which is the default. Overflow there is
    # silent and shows up as one wrong glyph, so refuse to emit instead.
    if idx >= 1 << 20:
        sys.exit("bitmap is over 1 MB, past LVGL's 20-bit bitmap_index")
    for g in glyphs:
        if not (g.adv_w < (1 << 12) and g.box_w < 256 and g.box_h < 256
                and -128 <= g.ofs_x <= 127 and -128 <= g.ofs_y <= 127):
            sys.exit(f"U+{g.cp:04X} does not fit the LVGL glyph descriptor")

    # Split into one contiguous ASCII run and a sparse tail, the way
    # lv_font_conv does. Anything that breaks the run starts the tail.
    ascii_run = []
    for g in glyphs:
        if g.cp == (ascii_run[-1].cp + 1 if ascii_run else 0x20):
            ascii_run.append(g)
        else:
            break
    sparse = glyphs[len(ascii_run):]

    o = []
    w = o.append
    w("/*******************************************************************************")
    w(f" * Size: {line_height} px")
    w(" * Bpp: 1")
    w(f" * Opts: {' '.join(argv)}")
    w(" *")
    w(" * Generated by tools/pcf_to_lvgl.py - do not edit by hand.")
    w(" ******************************************************************************/")
    w("")
    w("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
    w('#include "lvgl.h"')
    w("#else")
    w('#include "lvgl/lvgl.h"')
    w("#endif")
    w("")
    guard = name.upper()
    w(f"#ifndef {guard}")
    w(f"#define {guard} 1")
    w("#endif")
    w("")
    w(f"#if {guard}")
    w("")
    w("/*-----------------")
    w(" *    BITMAPS")
    w(" *----------------*/")
    w("")
    w("/*Store the image of the glyphs*/")
    w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for g in glyphs:
        ch = chr(g.cp)
        label = ch if 0x21 <= g.cp <= 0x7E and ch not in '"\\' else ""
        w(f"    /* U+{g.cp:04X} \"{label}\" */")
        for i in range(0, len(g.blob), 12):
            w("    " + " ".join(f"0x{b:x}," for b in g.blob[i:i + 12]))
        w("")
    w("};")
    w("")
    w("")
    w("/*---------------------")
    w(" *  GLYPH DESCRIPTION")
    w(" *--------------------*/")
    w("")
    w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    w("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}"
      " /* id = 0 reserved */,")
    for g in glyphs:
        w(f"    {{.bitmap_index = {g.index}, .adv_w = {g.adv_w}, .box_w = {g.box_w}, "
          f".box_h = {g.box_h}, .ofs_x = {g.ofs_x}, .ofs_y = {g.ofs_y}}},")
    w("};")
    w("")
    w("")
    w("/*---------------------")
    w(" *  CHARACTER MAPPING")
    w(" *--------------------*/")
    w("")
    if sparse:
        start = sparse[0].cp
        offsets = [g.cp - start for g in sparse]
        w("static const uint16_t unicode_list_1[] = {")
        for i in range(0, len(offsets), 8):
            w("    " + ", ".join(f"0x{v:x}" for v in offsets[i:i + 8]) + ("," if i + 8 < len(offsets) else ""))
        w("};")
        w("")
    w("/*Collect the unicode lists and glyph_id offsets*/")
    w("static const lv_font_fmt_txt_cmap_t cmaps[] =")
    w("{")
    w("    {")
    w(f"        .range_start = {ascii_run[0].cp}, .range_length = {len(ascii_run)}, "
      ".glyph_id_start = 1,")
    w("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, "
      ".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY")
    w("    }" + ("," if sparse else ""))
    if sparse:
        w("    {")
        w(f"        .range_start = {sparse[0].cp}, .range_length = {offsets[-1] + 1}, "
          f".glyph_id_start = {len(ascii_run) + 1},")
        w("        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, "
          f".list_length = {len(offsets)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY")
        w("    }")
    w("};")
    w("")
    w("")
    w("/*--------------------")
    w(" *  ALL CUSTOM DATA")
    w(" *--------------------*/")
    w("")
    w("#if LVGL_VERSION_MAJOR == 8")
    w("/*Store all the custom data of the font*/")
    w("static  lv_font_fmt_txt_glyph_cache_t cache;")
    w("#endif")
    w("")
    w("#if LVGL_VERSION_MAJOR >= 8")
    w("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    w("#else")
    w("static lv_font_fmt_txt_dsc_t font_dsc = {")
    w("#endif")
    w("    .glyph_bitmap = glyph_bitmap,")
    w("    .glyph_dsc = glyph_dsc,")
    w("    .cmaps = cmaps,")
    w("    .kern_dsc = NULL,")
    w("    .kern_scale = 0,")
    w(f"    .cmap_num = {2 if sparse else 1},")
    w("    .bpp = 1,")
    w("    .kern_classes = 0,")
    w("    .bitmap_format = 0,")
    w("#if LVGL_VERSION_MAJOR == 8")
    w("    .cache = &cache")
    w("#endif")
    w("};")
    w("")
    w("")
    w("/*-----------------")
    w(" *  PUBLIC FONT")
    w(" *----------------*/")
    w("")
    w("/*Initialize a public general font descriptor*/")
    w("#if LVGL_VERSION_MAJOR >= 8")
    w(f"const lv_font_t {name} = {{")
    w("#else")
    w(f"lv_font_t {name} = {{")
    w("#endif")
    w("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    "
      "/*Function pointer to get glyph's data*/")
    w("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    "
      "/*Function pointer to get glyph's bitmap*/")
    w(f"    .line_height = {line_height},          /*The maximum line height required by the font*/")
    w(f"    .base_line = {base_line},             /*Baseline measured from the bottom of the line*/")
    w("#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)")
    w("    .subpx = LV_FONT_SUBPX_NONE,")
    w("#endif")
    w("#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8")
    w("    .underline_position = -1,")
    w("    .underline_thickness = 1,")
    w("#endif")
    w("    .dsc = &font_dsc,          "
      "/*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */")
    w("#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9")
    w("    .fallback = NULL,")
    w("#endif")
    w("    .user_data = NULL,")
    w("};")
    w("")
    w("")
    w("")
    w(f"#endif /*#if {guard}*/")
    w("")
    with open(path, "w") as fh:
        fh.write("\n".join(o))


def parse_ranges(text):
    out = []
    for part in text.split(","):
        if "-" in part[1:]:
            lo, hi = part.split("-", 1)
            out.extend(range(int(lo, 0), int(hi, 0) + 1))
        else:
            out.append(int(part, 0))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcf", required=True)
    ap.add_argument("--size", type=int, required=True, help="pixel height of the strike")
    ap.add_argument("--name", required=True)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--range", default="0x20-0x7E,0xB0")
    ap.add_argument("--symbol-font")
    ap.add_argument("--symbols", default="")
    ap.add_argument("--symbol-size", type=int)
    ap.add_argument("--embolden", action="store_true",
                   help="widen stems by one pixel, for strikes with no bold cut")
    ap.add_argument("--scale", type=int, default=1,
                    help="integer pixel doubling factor")
    args = ap.parse_args()

    base = ImageFont.truetype(args.pcf, args.size)
    ascent, descent = base.getmetrics()
    if ascent + descent != args.size:
        print(f"warning: {args.pcf} strike is {ascent + descent}px, not {args.size}",
              file=sys.stderr)

    glyphs = build(base, sorted(set(parse_ranges(args.range))), ascent, descent,
                   False, scale=args.scale, bold=args.embolden)

    if args.symbol_font and args.symbols:
        sym = ImageFont.truetype(args.symbol_font, args.symbol_size or args.size)
        sym_asc, sym_desc = sym.getmetrics()
        # Icons carry one pixel of side bearing; the outline font's own advance
        # is far wider than the inked glyph once it is this small, and a gap
        # that big between a triangle and the word after it reads as a typo.
        glyphs += build(sym, sorted(set(parse_ranges(args.symbols))),
                        sym_asc, sym_desc, True, adv_override=lambda bw: bw + 1,
                        scale=args.scale)
        glyphs.sort(key=lambda g: g.cp)

    emit(args.output, args.name, glyphs,
         (ascent + descent) * args.scale, descent * args.scale, sys.argv[1:])
    print(f"  {args.name:12s} {len(glyphs)} glyphs, "
          f"{sum(len(g.packed()) for g in glyphs)} bitmap bytes")


if __name__ == "__main__":
    main()
