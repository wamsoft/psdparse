"""レイヤからの文書合成 (PSDFile.composite)。

Photoshop が保存した合成画像を正解として比べる。テキストレイヤの文書
(textboxsample.psd) と、グループ / ブレンドモード / マスク / 塗りつぶしを
組み合わせた小さな文書をその場で組み立てて確かめる。

開発時には psd-tools のテスト素材 (148 文書、効果と調整レイヤ無し) で照合している。
"""
import struct

import numpy as np
import pytest

import psdparse


def _composite(p, effects=False):
    h = p.header
    bg = (255, 255, 255) if not p.merged_has_transparency else None
    out, stats = p.composite(effects=effects, background=bg)
    return np.frombuffer(out, np.uint8).reshape(h.height, h.width, 4).astype(int), stats


def _merged(p):
    h = p.header
    return np.frombuffer(p.merged_image(), np.uint8).reshape(h.height, h.width, 4).astype(int)


def test_matches_photoshop_merged(sample_textbox_psd):
    p = psdparse.PSDFile()
    assert p.load(str(sample_textbox_psd))
    got, stats = _composite(p)
    ref = _merged(p)
    diff = np.abs(got[:, :, :3] - ref[:, :, :3]).max(axis=2)
    assert (diff > 12).mean() < 0.02   # テキストは過去の編集で画素と合成が少しずれている
    assert stats["skipped_adjustments"] == 0


def _raw_psd(w, h, layers, channels=3):
    """8bit RGB の PSD を組み立てる。layers: [(left, top, bgra_bytes, lw, lh, key, opacity)]"""
    recs, data = b"", b""
    for left, top, bgra, lw, lh, key, opacity in layers:
        planes = [bgra[3::4], bgra[2::4], bgra[1::4], bgra[0::4]]   # A, R, G, B
        chans = b""
        for cid, pl in zip((-1, 0, 1, 2), planes):
            chans += struct.pack(">hI", cid, 2 + len(pl))
            data += b"\0\0" + pl
        name = b"\x03lay"
        extra = struct.pack(">II", 0, 0) + name
        recs += (struct.pack(">iiii", top, left, top + lh, left + lw) + struct.pack(">H", 4) + chans
                 + b"8BIM" + key + bytes([opacity, 0, 0, 0]) + struct.pack(">I", len(extra)) + extra)
    body = struct.pack(">h", len(layers)) + recs + data
    if len(body) % 2:
        body += b"\0"
    lm = struct.pack(">I", len(body)) + body + struct.pack(">I", 0)
    hdr = b"8BPS" + struct.pack(">H6xHIIHH", 1, channels, h, w, 8, psdparse.COLOR_MODE_RGB)
    return (hdr + struct.pack(">I", 0) + struct.pack(">I", 0) + struct.pack(">I", len(lm)) + lm
            + b"\0\0" + bytes(w * h * channels))


def _solid(w, h, rgba):
    r, g, b, a = rgba
    return bytes([b, g, r, a]) * (w * h)


@pytest.mark.parametrize("key, expect", [
    (b"norm", (64, 128, 192)),
    (b"mul ", (round(200 * 64 / 255), round(100 * 128 / 255), round(50 * 192 / 255))),
    (b"scrn", (200 + 64 - round(200 * 64 / 255), 100 + 128 - round(100 * 128 / 255),
               50 + 192 - round(50 * 192 / 255))),
    (b"diff", (136, 28, 142)),
    (b"dark", (64, 100, 50)),
    (b"lite", (200, 128, 192)),
])
def test_blend_modes(tmp_path, key, expect):
    w = h = 4
    data = _raw_psd(w, h, [(0, 0, _solid(w, h, (200, 100, 50, 255)), w, h, b"norm", 255),
                           (0, 0, _solid(w, h, (64, 128, 192, 255)), w, h, key, 255)])
    path = tmp_path / "b.psd"
    path.write_bytes(data)
    p = psdparse.PSDFile()
    assert p.load(str(path))
    got, _ = _composite(p)
    r, g, b = got[0, 0, 2], got[0, 0, 1], got[0, 0, 0]
    assert all(abs(x - y) <= 1 for x, y in zip((r, g, b), expect)), ((r, g, b), expect)


def test_opacity_and_transparency(tmp_path):
    w = h = 4
    data = _raw_psd(w, h, [(1, 1, _solid(2, 2, (255, 0, 0, 255)), 2, 2, b"norm", 128)], channels=4)
    path = tmp_path / "t.psd"
    path.write_bytes(data)
    p = psdparse.PSDFile()
    assert p.load(str(path))
    out, _ = p.composite(effects=False)
    px = np.frombuffer(out, np.uint8).reshape(h, w, 4)
    assert tuple(px[0, 0]) == (0, 0, 0, 0)                 # レイヤの外は透明
    assert tuple(px[1, 1][[2, 3]]) == (255, 128)           # 赤、不透明度 50%
