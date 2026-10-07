"""未編集で保存したときに、元と同じバイト列へ戻るか (書き手ごとの形の揺れ)。

Photoshop 以外の書き手も含め、PSD には「仕様どおりだが形が揺れる」箇所がある。
それぞれの形を数十バイトの PSD を組み立てて再現し、load → save で 1 バイトも
変わらないことを確かめる。どれも以前は形を作り直してしまい一致しなかった。

  * カラーモードデータが空でない (Indexed のパレット、32bit の HDR toning)。
    読み取り範囲がファイル末尾まで伸びていて、保存時に残り全部を二重に書いていた。
  * layer & mask 情報がセクションごと空 (長さ 0)。
  * layer info が奇数長のまま (詰め物なし) で、global layer mask info の長さ
    フィールドが無く、2 バイトの詰め物だけが続く。

環境変数 PSDPARSE_CORPUS にフォルダを指定すると、その下の *.psd / *.psb 全部で
同じ確認をする (psd-tools のテスト素材などをローカルに置いて使う)。
"""
import os
import struct
from pathlib import Path

import pytest

import psdparse


def _header(channels, w, h, depth, mode, version=1):
    return b"8BPS" + struct.pack(">H6xHIIHH", version, channels, h, w, depth, mode)


def _section(body):
    return struct.pack(">I", len(body)) + body


def _roundtrip(tmp_path, data, name="in.psd"):
    src = tmp_path / name
    src.write_bytes(data)
    p = psdparse.PSDFile()
    assert p.load(str(src))
    dst = tmp_path / ("out" + src.suffix)
    assert p.save(str(dst))
    return p, dst.read_bytes()


def test_indexed_color_mode_data(tmp_path):
    palette = bytes(range(256)) * 3
    pixels = bytes([0, 1, 2, 3])
    data = (_header(1, 2, 2, 8, psdparse.COLOR_MODE_INDEXED)
            + _section(palette)
            + _section(b"")            # image resources
            + _section(b"")            # layer & mask: セクションごと空
            + b"\x00\x00" + pixels)    # image data (raw)
    p, out = _roundtrip(tmp_path, data)
    assert out == data
    assert len(p.color_table["colors"]) == 256
    assert p.layers == []


def test_odd_layer_info_without_global_mask_field(tmp_path):
    # 1x3 のグレーのレイヤ 1 枚。チャンネルは raw (2 + 3 バイト) なので
    # layer info 本体は奇数長 (59 バイト) になる。
    record = (struct.pack(">iiii", 0, 0, 1, 3)          # top, left, bottom, right
              + struct.pack(">H", 1) + struct.pack(">hI", 0, 5)
              + b"8BIMnorm" + bytes([255, 0, 0, 0])
              + _section(struct.pack(">II", 0, 0) + b"\x00\x00\x00\x00"))
    body = struct.pack(">h", 1) + record + b"\x00\x00" + bytes([10, 20, 30])
    assert len(body) % 2 == 1
    layer_mask = _section(body) + b"\x00\x00"   # 詰め物 2 バイト。global mask の長さは無い
    data = (_header(1, 3, 1, 8, psdparse.COLOR_MODE_GRAYSCALE)
            + _section(b"") + _section(b"")
            + _section(layer_mask)
            + b"\x00\x00" + bytes([10, 20, 30]))
    p, out = _roundtrip(tmp_path, data)
    assert out == data
    assert len(p.layers) == 1
    assert (p.layers[0].width, p.layers[0].height) == (3, 1)


def _corpus_files():
    root = os.environ.get("PSDPARSE_CORPUS")
    if not root:
        return []
    return sorted(str(f) for f in Path(root).rglob("*") if f.suffix.lower() in (".psd", ".psb"))


@pytest.mark.parametrize("path", _corpus_files() or [pytest.param(None, marks=pytest.mark.skip(
    reason="set PSDPARSE_CORPUS to a folder of PSD/PSB files"))])
def test_corpus_roundtrip(path, tmp_path):
    p = psdparse.PSDFile()
    assert p.load(path)
    dst = tmp_path / ("out" + Path(path).suffix)
    assert p.save(str(dst))
    assert dst.read_bytes() == Path(path).read_bytes()


DESC_SKIP = {"lfx2": 8, "SoLd": 12, "SoLE": 12, "vstk": 4, "vscg": 8, "vogk": 8,
             "SoCo": 4, "GdFl": 4, "PtFl": 4}


@pytest.mark.parametrize("path", _corpus_files() or [pytest.param(None, marks=pytest.mark.skip(
    reason="set PSDPARSE_CORPUS to a folder of PSD/PSB files"))])
def test_corpus_descriptor_rewrite(path, tmp_path):
    """読める descriptor を全部「変更なし」で書き戻して保存しても、元と一致する
    (descriptor の直列化と、レイヤの追加情報の組み立て直しが byte-exact)"""
    p = psdparse.PSDFile()
    assert p.load(path)
    touched = False
    for i, layer in enumerate(p.layers):
        for key in layer.info_keys:
            if key in DESC_SKIP and layer.descriptor(key) is not None:
                p.set_layer_descriptor(i, key, {}, DESC_SKIP[key])
                touched = True
    if not touched:
        pytest.skip("no descriptor blocks")
    dst = tmp_path / ("out" + Path(path).suffix)
    assert p.save(str(dst))
    assert dst.read_bytes() == Path(path).read_bytes()
