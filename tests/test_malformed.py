"""壊れた入力で落ちない (例外か False で返る) こと。

コミット済みの小さなサンプルを、途中で切ったもの・バイトを書き換えたものに
して読み込み、読めたら主な API をひととおり触る。プロセスが落ちればテストも
落ちるので、クラッシュの回帰を拾える。乱数は固定 (毎回同じ入力)。
"""
import random
import zlib

import pytest

import psdparse
from conftest import DATA

SAMPLES = ["masktest.psd", "maskparams.psd", "psbsample.psb", "lr16sample.psd",
           "vectorsample.psd", "smartsample.psd", "adjustsample.psd"]


def _touch(p, tmp_path):
    for name in ("slices", "guides", "layer_comps", "paths", "patterns", "linked_files",
                 "alpha_channels", "annotations", "global_layer_mask", "color_table"):
        try:
            getattr(p, name)
        except Exception:
            pass
    try:
        p.merged_image()
    except Exception:
        pass
    for i, l in enumerate(p.layers[:20]):
        for name in ("name", "text", "mask", "vector_mask", "smart_object", "adjustment",
                     "legacy_effects", "artboard", "effects", "fill"):
            try:
                getattr(l, name)
            except Exception:
                pass
        for mode in ("masked", "image", "mask"):
            try:
                p.layer_image(i, mode)
            except Exception:
                pass
    for i in range(len(p.patterns)):
        try:
            p.pattern_image(i)
        except Exception:
            pass
    for i in range(len(p.linked_files)):
        try:
            p.linked_file_data(i)
        except Exception:
            pass
    for c in range(min(p.header.channels, 8)):
        try:
            p.merged_channel(c)
        except Exception:
            pass
    try:
        p.save(str(tmp_path / "out.psd"))
    except Exception:
        pass


def _variants(data, seed):
    rng = random.Random(seed)
    n = len(data)
    cuts = sorted({26, 30, 40, n // 2, n - 1} | {rng.randrange(1, n) for _ in range(12)})
    for c in cuts:
        yield data[:c]
    for _ in range(12):
        b = bytearray(data)
        for _ in range(rng.randint(1, 6)):
            b[rng.randrange(n)] = rng.choice([0, 0xFF, 0x7F, 0x80, rng.randrange(256)])
        yield bytes(b)


@pytest.mark.parametrize("name", SAMPLES)
def test_damaged_inputs_do_not_crash(name, tmp_path):
    src = DATA / name
    if not src.is_file():
        pytest.skip(f"{name} not present")
    data = src.read_bytes()
    for k, variant in enumerate(_variants(data, zlib.crc32(name.encode()))):
        path = tmp_path / f"v{k}{src.suffix}"
        path.write_bytes(variant)
        p = psdparse.PSDFile()
        try:
            ok = p.load(str(path))
        except Exception:
            continue
        if ok:
            _touch(p, tmp_path)


def test_mask_mode_uses_mask_size(sample_maskparams_psd):
    """layer_image(i, 'mask') はマスク矩形の大きさで返る (以前はレイヤ矩形の
    大きさの領域に書いていて、マスクの方が大きいと範囲外へ書き込んでいた)"""
    p = psdparse.PSDFile()
    assert p.load(str(sample_maskparams_psd))
    for i, l in enumerate(p.layers):
        m = l.mask
        if not m or m["width"] <= 0:
            continue
        img = p.layer_image(i, "mask")
        assert len(img) == m["width"] * m["height"] * 4


def test_huge_declared_size_is_refused_before_allocating(tmp_path):
    """ヘッダが 300000x300000 を宣言しているのに画素データが数バイトしか無い
    ファイル。以前は merged_image() が幅x高さx4 (約 360GB) を確保しようとして
    いた。寸法がデータ量に見合わないので、確保する前に例外で断る。"""
    import struct
    w = h = 300000
    data = (b"8BPS" + struct.pack(">H6xHIIHH", 1, 3, h, w, 8, psdparse.COLOR_MODE_RGB)
            + struct.pack(">III", 0, 0, 0) + b"\x00\x01" + b"\x00" * 16)
    path = tmp_path / "huge.psd"
    path.write_bytes(data)
    p = psdparse.PSDFile()
    assert p.load(str(path))
    with pytest.raises(RuntimeError):
        p.merged_image()
    assert p.merged_channel(0) is None


def test_name_falls_back_when_not_utf8(sample_mask_psd, tmp_path):
    """Pascal 名が UTF-8 でなくても name は例外にしない (name_raw で生バイト)"""
    p = psdparse.PSDFile()
    assert p.load(str(sample_mask_psd))
    l = p.layers[0]
    assert isinstance(l.name, str)
    assert isinstance(l.name_raw, bytes)
