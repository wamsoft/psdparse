"""Tests for replacing the stored composite image (set_merged_image, 0.7.x)."""
import pytest

import psdparse


def _bgra_gradient(w, h):
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out += bytes([(x * 30) & 0xFF, (y * 40) & 0xFF, (x * 7 + y * 3) & 0xFF, 255])
    return bytes(out)


def _reload(p, tmp_path, name="m.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def test_set_merged_image_roundtrip(tmp_path):
    p = psdparse.PSDFile()
    p.create_blank(12, 9)
    data = _bgra_gradient(12, 9)
    p.set_merged_image(data)
    q = _reload(p, tmp_path)
    back = q.merged_image()
    # RGB channels round-trip (a 3-channel composite has no alpha -> opaque)
    def rgb(b):
        return [(b[i * 4 + 2], b[i * 4 + 1], b[i * 4 + 0]) for i in range(12 * 9)]
    assert rgb(back) == rgb(data)


def test_set_merged_image_bad_size_raises(tmp_path):
    p = psdparse.PSDFile()
    p.create_blank(12, 9)
    with pytest.raises(ValueError):
        p.set_merged_image(b"\x00" * 10)   # wrong length


def test_set_merged_image_valid_for_psd_tools(tmp_path):
    np = pytest.importorskip("numpy")
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    p = psdparse.PSDFile()
    p.create_blank(12, 9)
    data = _bgra_gradient(12, 9)
    p.set_merged_image(data)
    dst = tmp_path / "m.psd"
    assert p.save(str(dst))
    pt = PSDImage.open(str(dst))
    px = np.array(pt.topil())[0, 0]
    assert tuple(px[:3]) == (data[2], data[1], data[0])   # R,G,B of pixel 0
