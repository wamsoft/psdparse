"""合成画像のチャンネル: アルファ / スポットチャンネルの名前と表示設定
(image resource 1045 / 1006 / 1077)、merged_channel()、ZIP 圧縮の合成画像。

どれも数十バイトの PSD をその場で組み立てて確かめる。
"""
import struct
import zlib

import psdparse

W, H = 3, 2


def _header(channels, mode, depth=8):
    return b"8BPS" + struct.pack(">H6xHIIHH", 1, channels, H, W, depth, mode)


def _section(body):
    return struct.pack(">I", len(body)) + body


def _resource(rid, data):
    pad = b"\0" if len(data) % 2 else b""
    return b"8BIM" + struct.pack(">H", rid) + b"\0\0" + struct.pack(">I", len(data)) + data + pad


def _load(tmp_path, data, name="in.psd"):
    path = tmp_path / name
    path.write_bytes(data)
    p = psdparse.PSDFile()
    assert p.load(str(path))
    return p


def test_alpha_channel_names_and_display(tmp_path):
    names = b"".join(struct.pack(">I", len(n) + 1) + (n + "\0").encode("utf-16-be")
                     for n in ("Alpha 1", "特色 1"))
    disp = struct.pack(">I", 1)
    disp += struct.pack(">H4HHB", 0, 65535, 0, 0, 0, 50, 0)       # 選択範囲を赤 50%
    disp += struct.pack(">H4HHB", 2, 0, 65535, 65535, 65535, 100, 2)  # スポット
    planes = [bytes([10] * 6), bytes([20] * 6), bytes([30] * 6), bytes(range(6)), bytes([200] * 6)]
    data = (_header(5, psdparse.COLOR_MODE_RGB)
            + _section(b"")
            + _section(_resource(1045, names) + _resource(1077, disp))
            + _section(b"")
            + b"\0\0" + b"".join(planes))
    p = _load(tmp_path, data)
    a1, spot = p.alpha_channels
    assert (a1["plane"], a1["name"], a1["kind"], a1["opacity"]) == (3, "Alpha 1", "selected", 50)
    assert a1["color"] == (65535, 0, 0, 0)
    assert (spot["plane"], spot["name"], spot["kind"], spot["color_space"]) == (4, "特色 1", "spot", 2)
    assert p.merged_channel(3) == bytes(range(6))
    assert p.merged_channel(4) == bytes([200] * 6)
    assert p.merged_channel(0) == bytes([10] * 6)


def test_no_extra_channels(sample_mask_psd):
    p = psdparse.PSDFile()
    assert p.load(str(sample_mask_psd))
    extra = p.header.channels - 3
    assert len(p.alpha_channels) == extra


def _zip_psd(compression):
    gray = bytes([0, 50, 100, 150, 200, 250])
    alpha = bytes([255, 128, 0, 255, 128, 0])
    if compression == 3:   # 行ごとの差分 (予測付き ZIP)
        def delta(plane):
            out = bytearray()
            for y in range(H):
                row = plane[y * W:(y + 1) * W]
                out.append(row[0])
                out += bytes((row[x] - row[x - 1]) & 0xFF for x in range(1, W))
            return bytes(out)
        body = delta(gray) + delta(alpha)
    else:
        body = gray + alpha
    data = (_header(2, psdparse.COLOR_MODE_GRAYSCALE)
            + _section(b"") + _section(b"") + _section(b"")
            + struct.pack(">H", compression) + zlib.compress(body))
    return data, gray, alpha


def test_merged_zip_splits_channels(tmp_path):
    """合成画像の ZIP は全チャンネルで 1 本のストリーム。以前はチャンネルごとに
    先頭から展開していて、2 枚目以降が 1 枚目と同じになっていた。"""
    for compression in (2, 3):
        data, gray, alpha = _zip_psd(compression)
        p = _load(tmp_path, data, f"zip{compression}.psd")
        assert p.merged_channel(0) == gray, compression
        assert p.merged_channel(1) == alpha, compression
        img = p.merged_image()
        assert img[4 * 5:4 * 5 + 3] == bytes([250, 250, 250])   # 色は 1 枚目から
