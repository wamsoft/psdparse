"""Tests for pixel editing (E4, 0.7.0):

- PackBits(RLE) encoder round-trip (encode -> save -> reload -> decode == input),
- set_layer_pixels on an existing layer,
- add_layer (new image layer) with bbox / unicode name / blend / opacity,
- input validation and the 8-bit-RGB-only restriction,
- the save-over-a-loaded-file guard (mmap keeps the source read-only).

All pixel data is BGRA (the same interleave layer_image / merged_image use).
"""
import shutil

import pytest

import psdparse


def _bgra(w, h, kind="rand", seed=0):
    out = bytearray()
    st = seed
    for i in range(w * h):
        if kind == "same":
            out += bytes([7, 7, 7, 255])
        elif kind == "runs":
            out += bytes([(i // 10) & 0xFF] * 3 + [255])
        else:  # pseudo-random but deterministic
            st = (st * 1103515245 + 12345) & 0x7FFFFFFF
            out += bytes([st & 0xFF, (st >> 8) & 0xFF, (st >> 16) & 0xFF, 255])
    return bytes(out)


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


@pytest.mark.parametrize("w,h,kind", [
    (1, 1, "rand"), (1, 40, "rand"), (40, 1, "rand"),
    (16, 16, "same"), (17, 13, "rand"), (129, 1, "runs"), (100, 60, "rand"),
])
def test_packbits_roundtrip(psd_group, tmp_path, w, h, kind):
    src = _bgra(w, h, kind, seed=w * 100 + h)
    ni = psd_group.add_layer("enc", 0, 0, src, w, h)
    q = _reload(psd_group, tmp_path, f"rt_{w}x{h}.psd")
    assert q.layer_image(ni, "image") == src


def test_set_layer_pixels(psd_maskparams, tmp_path):
    w, h = 16, 16
    src = _bgra(w, h, "rand", seed=5)
    psd_maskparams.set_layer_pixels(0, src, w, h)
    q = _reload(psd_maskparams, tmp_path)
    assert q.layer_image(0, "image") == src


def test_add_layer_metadata(psd_group, tmp_path):
    n0 = len(psd_group.layers)
    w, h = 32, 24
    src = _bgra(w, h, "rand", seed=99)
    ni = psd_group.add_layer("新規レイヤ😀", 100, 50, src, w, h, "mul ", 200)
    assert ni == n0
    q = _reload(psd_group, tmp_path)
    assert len(q.layers) == n0 + 1
    L = q.layers[ni]
    assert (L.left, L.top, L.right, L.bottom) == (100, 50, 132, 74)
    assert L.name_unicode == "新規レイヤ😀"          # luni survives, incl. emoji
    assert L.blend_mode == psdparse.BlendMode.MULTIPLY
    assert L.opacity == 200
    assert q.layer_image(ni, "image") == src


def test_add_layer_bad_size_raises(psd_group):
    with pytest.raises(ValueError):
        psd_group.add_layer("x", 0, 0, b"\x00" * 10, 16, 16)  # 10 != 16*16*4


def test_set_layer_pixels_bad_size_raises(psd_maskparams):
    with pytest.raises(ValueError):
        psd_maskparams.set_layer_pixels(0, b"\x00" * 10, 16, 16)


def test_non_rgb_document_rejected(psd_gray):
    # graysample.psd is grayscale — pixel editing is 8-bit-RGB only.
    with pytest.raises(RuntimeError):
        psd_gray.add_layer("x", 0, 0, _bgra(4, 4), 4, 4)


def test_save_over_loaded_file_fails(sample_maskparams_psd, tmp_path):
    # Saving over a file that is currently mmap'd (e.g. the one you loaded)
    # must fail cleanly (return False), never corrupt it.
    work = tmp_path / "self.psd"
    shutil.copy(sample_maskparams_psd, work)
    p = psdparse.PSDFile()
    assert p.load(str(work))
    p.layers[0].opacity = 42
    assert p.save(str(work)) is False          # refused, source still intact
    q = psdparse.PSDFile()
    assert q.load(str(work))                   # original still loads


def test_added_layer_valid_for_psd_tools(psd_group, tmp_path):
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    w, h = 24, 20
    src = _bgra(w, h, "rand", seed=7)
    psd_group.add_layer("pt-check", 10, 10, src, w, h)
    dst = tmp_path / "pt.psd"
    assert psd_group.save(str(dst))
    pt = PSDImage.open(str(dst))
    pt.composite()                              # full decode must not raise
    assert "pt-check" in [l.name for l in pt.descendants()]
