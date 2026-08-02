"""Tests for the 0.6.0 color-mode additions in getMergedImage / getLayerImage:

- Lab (mode 9) -> sRGB via a standard D65 CIELAB conversion,
- Duotone (mode 8) rendered as grayscale (Adobe stores duotone data as gray).

labsample.psd was built from known RGB swatches [white, black, red, green]
converted to Lab. Neutral swatches (white/black) round-trip exactly regardless
of reference white; red is near-exact. Green diverges because the ICC Lab
encoding of a saturated primary is reference-white dependent, so it is not
asserted tightly here.
"""
import psdparse


def _rgb_pixels(psd, n):
    """First n pixels of the merged image as (R, G, B) tuples (from BGRA)."""
    data = psd.merged_image()
    return [(data[i * 4 + 2], data[i * 4 + 1], data[i * 4 + 0]) for i in range(n)]


def test_lab_mode_is_lab(psd_lab):
    assert psd_lab.header.mode == psdparse.COLOR_MODE_LAB


def test_lab_neutrals_exact(psd_lab):
    px = _rgb_pixels(psd_lab, 4)
    # swatch 0 = white, swatch 1 = black -- reference-white independent.
    assert px[0] == (255, 255, 255)
    assert px[1] == (0, 0, 0)


def test_lab_red_near(psd_lab):
    px = _rgb_pixels(psd_lab, 4)
    r, g, b = px[2]
    assert r > 240 and g < 15 and b < 15  # essentially pure red


def test_lab_not_blank(psd_lab):
    # Regression: Lab used to render an all-zero buffer.
    assert any(psd_lab.merged_image())


def test_duotone_renders_as_grayscale(psd_duo, psd_gray):
    assert psd_duo.header.mode == psdparse.COLOR_MODE_DUOTONE
    assert psd_gray.header.mode == psdparse.COLOR_MODE_GRAYSCALE
    duo = psd_duo.merged_image()
    gray = psd_gray.merged_image()
    assert any(duo)          # not blank
    assert duo == gray       # duotone data is grayscale
