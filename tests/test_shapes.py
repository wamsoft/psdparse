"""シェイプとパス: layer.shape / PSDFile.shape_mask と、パスのラスタライズ
(psdparse.flatten_path / rasterize_path / stroke_path)、合成でのシェイプの線。

線の描き方 (位置・端・角・破線・pt の線幅) は psd-tools のテスト素材 (Photoshop が
保存した合成画像) と照合して合わせている。ここでは幾何の性質と、Photoshop が
保存したシェイプ (config.psd) の画素との一致を確かめる。
"""
import math
import os
import struct

import numpy as np
import pytest

import psdparse

HERE = os.path.dirname(os.path.abspath(__file__))
SQUARE = {"subpaths": [{"closed": True, "knots": [(10, 10), (50, 10), (50, 50), (10, 50)]}]}


def plane(b, w, h):
    return np.frombuffer(b, np.uint8).reshape(h, w)


# --- パスのラスタライズ (モジュール関数) ---------------------------------------
def test_flatten_straight_and_curve():
    sub = psdparse.flatten_path(SQUARE)[0]
    assert sub["closed"] and sub["points"] == [(10, 10), (50, 10), (50, 50), (10, 50)]
    # 円 (4 本のベジェ): 折れ線の点は半径 r から tolerance 以内
    k = 0.5522847498 * 20
    circle = [{"anchor": (50, 30), "preceding": (50, 30 - k), "leaving": (50, 30 + k)},
              {"anchor": (30, 50), "preceding": (30 + k, 50), "leaving": (30 - k, 50)},
              {"anchor": (10, 30), "preceding": (10, 30 + k), "leaving": (10, 30 - k)},
              {"anchor": (30, 10), "preceding": (30 - k, 10), "leaving": (30 + k, 10)}]
    pts = psdparse.flatten_path([{"knots": circle}], tolerance=0.05)[0]["points"]
    assert len(pts) > 16
    assert all(abs(math.hypot(x - 30, y - 30) - 20) < 0.06 for x, y in pts)
    # 面積: 内接する折れ線のぶん (既定の tolerance 0.1px) だけ小さい
    m = plane(psdparse.rasterize_path([{"knots": circle}], 60, 60), 60, 60)
    assert -10 < m.sum() / 255 - math.pi * 400 < 1


def test_rasterize_offset_and_operations():
    m = plane(psdparse.rasterize_path(SQUARE, 60, 60), 60, 60)
    assert m.sum() // 255 == 1600 and m[30, 30] == 255 and m[5, 5] == 0
    # left/top でずらす
    m = plane(psdparse.rasterize_path(SQUARE, 20, 20, left=40, top=40), 20, 20)
    assert m[5, 5] == 255 and m[15, 15] == 0
    # 2 本目を差 (2) にすると穴
    hole = {"subpaths": SQUARE["subpaths"] + [
        {"closed": True, "operation": 2, "knots": [(20, 20), (40, 20), (40, 40), (20, 40)]}]}
    hole["subpaths"][0] = dict(hole["subpaths"][0], operation=1)
    m = plane(psdparse.rasterize_path(hole, 60, 60), 60, 60)
    assert m[30, 30] == 0 and m[15, 15] == 255


@pytest.mark.parametrize("align,inside,outside", [
    ("center", (10, 12), (8, 10)), ("inside", (10, 14), None), ("outside", None, (6, 10))])
def test_stroke_alignment(align, inside, outside):
    m = plane(psdparse.stroke_path(SQUARE, 60, 60, line_width=4, alignment=align), 60, 60)
    row = m[30]
    ink = set(np.nonzero(row[:30] == 255)[0].tolist())
    want = set()
    if inside: want |= set(range(*inside))
    if outside: want |= set(range(*outside))
    assert ink == want


def test_stroke_join_and_cap():
    # マイター: 角の外側まで埋まる / ベベル: 角が欠ける
    miter = plane(psdparse.stroke_path(SQUARE, 60, 60, line_width=6, join="miter"), 60, 60)
    bevel = plane(psdparse.stroke_path(SQUARE, 60, 60, line_width=6, join="bevel"), 60, 60)
    assert miter[7, 7] == 255 and bevel[7, 7] == 0
    line = [{"closed": False, "knots": [(10, 30), (50, 30)]}]
    butt = plane(psdparse.stroke_path(line, 60, 60, line_width=6), 60, 60)
    square = plane(psdparse.stroke_path(line, 60, 60, line_width=6, cap="square"), 60, 60)
    assert butt[30, 8] == 0 and square[30, 8] == 255
    assert square[30, 52] == 255 and butt[30, 52] == 0


def test_stroke_dashes():
    line = [{"closed": False, "knots": [(10, 30), (50, 30)]}]
    m = plane(psdparse.stroke_path(line, 60, 60, line_width=2, dashes=[5, 5]), 60, 60)
    assert m[30, 12] == 255 and m[30, 17] == 0 and m[30, 22] == 255
    # 長さ 0 + 丸い端 = 点線。両端にも点が付く
    m = plane(psdparse.stroke_path(line, 60, 60, line_width=6, cap="round", dashes=[0, 10]), 60, 60)
    cols = np.nonzero(m[30] > 128)[0]
    centers = [c for c in (10, 20, 30, 40, 50) if m[30, c] == 255]
    assert centers == [10, 20, 30, 40, 50] and m[30, 15] == 0 and cols.min() == 7


def test_bad_arguments():
    with pytest.raises(ValueError):
        psdparse.stroke_path(SQUARE, 10, 10, cap="pointy")
    with pytest.raises(ValueError):
        psdparse.rasterize_path(SQUARE, 0, 10)


# --- Photoshop が保存したシェイプ ---------------------------------------------
def test_config_shape_matches_stored_pixels():
    p = psdparse.PSDFile()
    assert p.load(os.path.join(HERE, "data", "config.psd"))
    shapes = [i for i, l in enumerate(p.layers) if l.shape]
    assert shapes
    for i in shapes:
        l = p.layers[i]
        s = l.shape
        assert s["fill"]["kind"] == "solid" and s["stroke"]["alignment"] == "center"
        assert s["origins"][0]["type"] == "rectangle"
        mask, x, y, w, h = p.shape_mask(i, "fill")
        assert p.shape_mask(i, "stroke") is None          # 線は無効
        cov = np.zeros((p.header.height, p.header.width), np.uint8)
        m = plane(mask, w, h)
        x0, y0 = max(0, x), max(0, y)
        cov[y0:y + h, x0:x + w] = m[y0 - y:, x0 - x:]
        alpha = np.frombuffer(p.layer_image(i, "image"), np.uint8).reshape(l.height, l.width, 4)[:, :, 3]
        got = cov[l.top:l.bottom, l.left:l.right]
        diff = np.abs(got.astype(int) - alpha)
        assert diff.max() <= 10 and diff.mean() < 0.5         # アンチエイリアスの差だけ


# --- 合成でのシェイプの線 (自前で組み立てた PSD) -------------------------------
def u(s):
    b = s.encode("utf-16-be")
    return struct.pack(">I", len(b) // 2) + b


def cid(s):
    b = s.encode("ascii")
    return struct.pack(">I", 0 if len(b) == 4 else len(b)) + b


def desc(class_id, items):
    body = u("\0") + cid(class_id) + struct.pack(">I", len(items))
    for k, v in items:
        body += cid(k) + v
    return body


def objc(c, items): return b"Objc" + desc(c, items)
def boolean(v): return b"bool" + bytes([1 if v else 0])
def enum(t, e): return b"enum" + cid(t) + cid(e)
def unit(u_, v): return b"UntF" + u_ + struct.pack(">d", v)
def double(v): return b"doub" + struct.pack(">d", v)
def rgb(r, g, b): return objc("RGBC", [("Rd  ", double(r)), ("Grn ", double(g)), ("Bl  ", double(b))])


def path_records(rect, W, H):
    def fx(v): return struct.pack(">i", int(round(v * (1 << 24))))
    l, t, r, b = rect
    out = struct.pack(">H", 6) + bytes(24)
    out += struct.pack(">H", 8) + struct.pack(">H", 0) + bytes(22)
    out += struct.pack(">HHh", 0, 4, 1) + bytes(20)
    for x, y in ((l, t), (r, t), (r, b), (l, b)):
        pt = fx(y / H) + fx(x / W)
        out += struct.pack(">H", 2) + pt * 3
    return out


def shape_psd(tmp_path, W, H, rect, fill_rgb, stroke_rgb, width, align, fill_enabled=True):
    vsms = struct.pack(">II", 3, 0) + path_records(rect, W, H)
    vscg = b"SoCo" + struct.pack(">I", 16) + desc("null", [("Clr ", rgb(*fill_rgb))])
    vstk = struct.pack(">I", 16) + desc("strokeStyle", [
        ("strokeStyleVersion", b"long" + struct.pack(">i", 2)),
        ("strokeEnabled", boolean(True)), ("fillEnabled", boolean(fill_enabled)),
        ("strokeStyleLineWidth", unit(b"#Pxl", width)),
        ("strokeStyleLineAlignment", enum("strokeStyleLineAlignment", align)),
        ("strokeStyleOpacity", unit(b"#Prc", 100)),
        ("strokeStyleBlendMode", enum("BlnM", "normal")),
        ("strokeStyleContent", objc("solidColorLayer", [("Clr ", rgb(*stroke_rgb))]))])
    extra = struct.pack(">II", 0, 0) + b"\x05shape" + b"\0\0"
    for key, payload in ((b"vsms", vsms), (b"vscg", vscg), (b"vstk", vstk)):
        if len(payload) % 2:
            payload += b"\0"
        extra += b"8BIM" + key + struct.pack(">I", len(payload)) + payload
    # 画素を持たないシェイプ (矩形 0、各チャンネルは圧縮種別だけ)
    chans = b"".join(struct.pack(">hI", c, 2) for c in (-1, 0, 1, 2))
    rec = (struct.pack(">iiii", 0, 0, 0, 0) + struct.pack(">H", 4) + chans + b"8BIMnorm"
           + bytes([255, 0, 0, 0]) + struct.pack(">I", len(extra)) + extra)
    body = struct.pack(">h", -1) + rec + b"\0\0" * 4
    if len(body) % 2:
        body += b"\0"
    lm = struct.pack(">I", len(body)) + body + struct.pack(">I", 0)
    hdr = b"8BPS" + struct.pack(">H6xHIIHH", 1, 4, H, W, 8, psdparse.COLOR_MODE_RGB)
    psd = (hdr + struct.pack(">I", 0) + struct.pack(">I", 0) + struct.pack(">I", len(lm)) + lm
           + b"\0\0" + bytes(W * H * 4))
    path = tmp_path / "shape.psd"
    path.write_bytes(psd)
    p = psdparse.PSDFile()
    assert p.load(str(path))
    return p


@pytest.mark.parametrize("align,first_stroke_col", [
    ("strokeStyleAlignOutside", 6), ("strokeStyleAlignInside", 10), ("strokeStyleAlignCenter", 8)])
def test_composite_draws_shape_stroke(tmp_path, align, first_stroke_col):
    p = shape_psd(tmp_path, 40, 40, (10, 10, 30, 30), (255, 0, 0), (0, 0, 255), 4, align)
    s = p.layers[0].shape
    assert s["stroke"]["width"] == 4 and s["fill"]["kind"] == "solid"
    out, _ = p.composite()
    img = np.frombuffer(out, np.uint8).reshape(40, 40, 4)
    row = img[20]
    blue = [x for x in range(20) if tuple(row[x]) == (255, 0, 0, 255)]
    assert blue == list(range(first_stroke_col, first_stroke_col + 4))
    assert tuple(row[18]) == (0, 0, 255, 255)                 # 中は赤い塗り
    assert row[first_stroke_col - 1, 3] == 0


def test_composite_stroke_only_shape(tmp_path):
    p = shape_psd(tmp_path, 40, 40, (10, 10, 30, 30), (255, 0, 0), (0, 0, 255), 2,
                  "strokeStyleAlignCenter", fill_enabled=False)
    out, _ = p.composite()
    img = np.frombuffer(out, np.uint8).reshape(40, 40, 4)
    assert img[20, 20, 3] == 0                                  # 塗りは無効
    assert tuple(img[20, 9]) == (255, 0, 0, 255)
    mask, x, y, w, h = p.shape_mask(0, "stroke")
    m = plane(mask, w, h)
    assert m[20 - y, 9 - x] == 255 and m[20 - y, 20 - x] == 0
