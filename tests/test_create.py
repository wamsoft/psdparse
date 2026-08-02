"""Tests for from-scratch PSD construction (E5, 0.7.0):

PSDFile.create_blank(w, h) initializes an empty 8-bit RGB document (white
composite); add_layer(...) then builds it up and save() writes a valid PSD.
No fixture PSD is needed — these build everything from bytes.
"""
import pytest

import psdparse


def _bgra(w, h, r, g, b, a=255):
    return bytes([b, g, r, a]) * (w * h)


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def test_create_blank_header():
    p = psdparse.PSDFile()
    p.create_blank(64, 48)
    assert p.is_loaded
    assert p.header.width == 64 and p.header.height == 48
    assert p.header.mode == psdparse.COLOR_MODE_RGB
    assert p.header.depth == 8
    assert len(p.layers) == 0


def test_create_blank_empty_roundtrips(tmp_path):
    p = psdparse.PSDFile()
    p.create_blank(64, 48)
    q = _reload(p, tmp_path)
    assert len(q.layers) == 0
    assert q.header.width == 64 and q.header.height == 48


def test_create_from_scratch(tmp_path):
    p = psdparse.PSDFile()
    p.create_blank(100, 80)
    bg = _bgra(100, 80, 30, 60, 90)
    red = _bgra(40, 30, 255, 0, 0)
    i0 = p.add_layer("bg", 0, 0, bg, 100, 80)
    i1 = p.add_layer("red-box", 20, 15, red, 40, 30, "norm", 200)
    assert (i0, i1) == (0, 1)
    q = _reload(p, tmp_path)
    assert len(q.layers) == 2
    L = q.layers[i1]
    assert L.name_unicode == "red-box"
    assert (L.left, L.top, L.right, L.bottom) == (20, 15, 60, 45)
    assert L.opacity == 200
    assert q.layer_image(i0, "image") == bg
    assert q.layer_image(i1, "image") == red


def test_create_blank_rejects_non_rgb():
    p = psdparse.PSDFile()
    with pytest.raises(ValueError):
        p.create_blank(32, 32, psdparse.COLOR_MODE_GRAYSCALE)


def test_create_blank_rejects_bad_size():
    p = psdparse.PSDFile()
    with pytest.raises(ValueError):
        p.create_blank(0, 10)


def test_created_file_valid_for_psd_tools(tmp_path):
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    p = psdparse.PSDFile()
    p.create_blank(48, 32)
    p.add_layer("solid", 4, 4, _bgra(20, 16, 10, 200, 30), 20, 16)
    dst = tmp_path / "scratch.psd"
    assert p.save(str(dst))
    pt = PSDImage.open(str(dst))
    pt.composite()
    assert pt.size == (48, 32)
    assert "solid" in [l.name for l in pt.descendants()]
