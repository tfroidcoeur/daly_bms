# Fonts

The UI is set in **X11 misc-fixed**, the bitmap family behind u8g2's
`u8g2_font_6x13_tf` (which Waveshare's own example for this board uses). On a
1 bpp panel a font designed at one bit beats an outline font rasterised down to
one: every stem is a whole pixel, so no edge depends on a threshold.

## The ladder

`UI_FONT_S`/`M`/`L`/`XL`/`XXL` in `ui/fonts/fonts.h`: 6x13, 7x14, 9x15, 10x20,
and 6x13 bold doubled for the `NO DATA` alert. Everything below XL has a real
bold cut, which carries warning emphasis.

Because the fonts are monospaced, whether a string fits its column is
arithmetic. `ui/page_pack.c` states each column's character budget and checks
the widest possible value against it with `UI_STATIC_ASSERT`, so a reworded
string fails the build instead of overlapping its neighbour.

## Regenerating

```bash
sudo apt install xfonts-base
tools/make-fonts.sh
```

misc-fixed ships as PCF, which `lv_font_conv` rejects (*Unsupported OpenType
signature fcp*). `tools/pcf_to_lvgl.py` reads the strike through FreeType
instead and emits the same table `lv_font_conv` would. The `LV_SYMBOL_*`
glyphs have no bitmap equivalent; they are merged in from FontAwesome,
thresholded.
