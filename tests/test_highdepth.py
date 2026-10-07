"""PSB (large document format) と 16/32bit レイヤ (Lr16 / Lr32) の読み書き。

サンプルは tests/data/make_highdepth.py が psd-tools で作ったもの:
  psbsample.psb   8bit PSB。赤 (3,2)-(13,10) / 緑 (8,6)-(20,18)
  lr16sample.psd  16bit。レイヤは文書末尾の Lr16 ブロックにある
  lr32sample.psd  32bit。レイヤは Lr32 (赤 1 枚)
  lr16sample.psb  16bit PSB。Lr16 は 8 バイト長のキー

PSB では section / layer info / チャンネル長と一部の追加情報の長さが 8 バイト、
RLE の行バイト数が 4 バイトになる。16/32bit 文書では Photoshop がレイヤを
Lr16 / Lr32 に置き、layer info 本体は空にする。どちらも psd-tools を相手に
画素と構造を照合する。
"""
import hashlib

import pytest

import psdparse
from conftest import DATA

psd_tools = pytest.importorskip("psd_tools")

SAMPLES = {
    "psbsample.psb": (2, 8, None),
    "lr16sample.psd": (1, 16, "Lr16"),
    "lr32sample.psd": (1, 32, "Lr32"),
    "lr16sample.psb": (2, 16, "Lr16"),
}
RED = (3, 2, 13, 10)
GREEN = (8, 6, 20, 18)


def _load(name):
    path = DATA / name
    if not path.is_file():
        pytest.skip(f"{name} not generated (run tests/data/make_highdepth.py)")
    p = psdparse.PSDFile()
    assert p.load(str(path))
    return p, path


def _reload(p, tmp_path, name):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q, dst


def _first_pixel(p, i, mode="masked"):
    """レイヤ左上の BGRA"""
    return tuple(p.layer_image(i, mode)[:4])


def _bbox(l):
    return (l.left, l.top, l.right, l.bottom)


@pytest.mark.parametrize("name", list(SAMPLES))
def test_header_and_layer_source(name):
    p, _ = _load(name)
    version, depth, source = SAMPLES[name]
    assert p.header.version == version
    assert p.header.is_psb == (version == 2)
    assert p.header.depth == depth
    assert p.layer_source == source


@pytest.mark.parametrize("name", list(SAMPLES))
def test_layers_and_pixels(name):
    p, _ = _load(name)
    names = [l.name for l in p.layers]
    assert names[0] == "red"
    assert _bbox(p.layers[0]) == RED
    assert _first_pixel(p, 0, "image") == (0, 0, 255, 255)
    assert _first_pixel(p, 0) == (0, 0, 255, 255)        # マスク込みでも不透明
    if len(names) > 1:
        assert names[1] == "green"
        assert _bbox(p.layers[1]) == GREEN
        assert _first_pixel(p, 1) == (0, 255, 0, 255)


@pytest.mark.parametrize("name", list(SAMPLES))
def test_unmodified_roundtrip_identical(name, tmp_path):
    p, src = _load(name)
    dst = tmp_path / name
    assert p.save(str(dst))
    assert hashlib.md5(src.read_bytes()).digest() == hashlib.md5(dst.read_bytes()).digest()


@pytest.mark.parametrize("name", list(SAMPLES))
def test_structural_edit_rewrites_layer_block(name, tmp_path):
    """削除 + 改名 + 不透明度変更 → 保存。Lr16/Lr32 ならそのブロックが書き直され、
    PSB なら 8 バイト長のまま書かれていることを psd-tools で確かめる。"""
    p, _ = _load(name)
    p.layers[0].opacity = 128
    p.set_layer_name(0, "赤")
    if len(p.layers) > 1:
        p.delete_layer(1)
    q, dst = _reload(p, tmp_path, name)
    assert q.layer_source == SAMPLES[name][2]
    assert [l.name_unicode for l in q.layers] == ["赤"]
    assert q.layers[0].opacity == 128
    assert _first_pixel(q, 0) == (0, 0, 255, 255)

    t = psd_tools.PSDImage.open(dst)
    assert t.version == SAMPLES[name][0]
    layers = list(t)
    assert [l.name for l in layers] == ["赤"]
    assert layers[0].opacity == 128
    assert layers[0].bbox == RED
    px = layers[0].numpy()[0, 0]
    assert tuple(round(float(v), 3) for v in px) == (1.0, 0.0, 0.0, 1.0)


def test_psb_pixel_edit_uses_wide_rle_counts(tmp_path):
    """PSB で画素を差し替える / レイヤを足すと、RLE の行バイト数は 4 バイトで
    書かれる (2 バイトで書くと psd-tools も Photoshop も読めない)。"""
    p, _ = _load("psbsample.psb")
    w, h = 6, 5
    blue = bytes([255, 0, 0, 255]) * (w * h)                 # BGRA
    p.set_layer_pixels(0, blue, w, h)
    yellow = bytes([0, 255, 255, 255]) * (4 * 3)
    idx = p.add_layer("yellow", 20, 15, yellow, 4, 3)
    assert p.set_merged_image_solid(10, 20, 30)
    q, dst = _reload(p, tmp_path, "edit.psb")
    assert _first_pixel(q, 0, "image") == (255, 0, 0, 255)
    assert _first_pixel(q, idx, "image") == (0, 255, 255, 255)
    merged = q.merged_image()
    assert tuple(merged[:3]) == (30, 20, 10)

    t = psd_tools.PSDImage.open(dst)
    layers = list(t)
    assert tuple(float(v) for v in layers[0].numpy()[0, 0][:3]) == (0.0, 0.0, 1.0)
    assert layers[idx].name == "yellow"
    assert layers[idx].bbox == (20, 15, 24, 18)
    assert tuple(float(v) for v in layers[idx].numpy()[0, 0][:3]) == (1.0, 1.0, 0.0)
    assert t.topil().getpixel((0, 0))[:3] == (10, 20, 30)


def test_copy_layer_between_psd_and_psb_is_refused():
    """チャンネルは符号化したまま持ち込むので、PSD ↔ PSB や深度違いは不可"""
    psb, _ = _load("psbsample.psb")
    lr16, _ = _load("lr16sample.psd")
    lr16b, _ = _load("lr16sample.psb")
    with pytest.raises(ValueError):
        psb.copy_layer_from(lr16, 0)            # 深度違い
    with pytest.raises(ValueError):
        lr16.copy_layer_from(lr16b, 0)          # PSD ← PSB
    assert lr16.copy_layer_from(lr16, 1) >= 0   # 同じ形式なら可
