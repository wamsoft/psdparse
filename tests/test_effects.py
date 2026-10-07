"""レイヤー効果の描画 (composite(effects=True) / render_layer)。

lfx2 (効果の descriptor) を自前で組み立てた小さな PSD で、効果ごとの性質を
確かめる。Photoshop との見た目の一致は開発時に psd-tools のテスト素材
(Photoshop が保存した合成画像) と照合して合わせている (効果ごとのぼかし幅、
光彩の範囲など)。
"""
import struct

import numpy as np

import psdparse


# --- descriptor の組み立て ---------------------------------------------------
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
def rgb(r, g, b): return objc("RGBC", [("Rd  ", b"doub" + struct.pack(">d", r)),
                                      ("Grn ", b"doub" + struct.pack(">d", g)),
                                      ("Bl  ", b"doub" + struct.pack(">d", b))])


def lfx2(effects):
    items = [("Scl ", unit(b"#Prc", 100)), ("masterFXSwitch", boolean(True))] + effects
    return struct.pack(">II", 0, 16) + desc("null", items)


def stroke(size, color):
    return ("FrFX", objc("FrFX", [("enab", boolean(True)), ("Styl", enum("FStl", "OutF")),
                                  ("PntT", enum("FrFl", "SClr")), ("Md  ", enum("BlnM", "Nrml")),
                                  ("Opct", unit(b"#Prc", 100)), ("Sz  ", unit(b"#Pxl", size)),
                                  ("Clr ", rgb(*color))]))


def drop_shadow(angle, distance, size, color):
    return ("DrSh", objc("DrSh", [("enab", boolean(True)), ("Md  ", enum("BlnM", "Nrml")),
                                  ("Clr ", rgb(*color)), ("Opct", unit(b"#Prc", 100)),
                                  ("uglg", boolean(False)), ("lagl", unit(b"#Ang", angle)),
                                  ("Dstn", unit(b"#Pxl", distance)), ("Ckmt", unit(b"#Prc", 100)),
                                  ("blur", unit(b"#Pxl", size)), ("layerConceals", boolean(True))]))


def color_overlay(color):
    return ("SoFi", objc("SoFi", [("enab", boolean(True)), ("Md  ", enum("BlnM", "Nrml")),
                                  ("Opct", unit(b"#Prc", 100)), ("Clr ", rgb(*color))]))


# --- PSD の組み立て (8bit RGB、透明度あり) ------------------------------------
def build(tmp_path, w, h, rect, color, blocks, fill=255):
    left, top, right, bottom = rect
    lw, lh = right - left, bottom - top
    planes = [bytes([255]) * (lw * lh), bytes([color[0]]) * (lw * lh),
              bytes([color[1]]) * (lw * lh), bytes([color[2]]) * (lw * lh)]
    chans, data = b"", b""
    for cid_, pl in zip((-1, 0, 1, 2), planes):
        chans += struct.pack(">hI", cid_, 2 + len(pl))
        data += b"\0\0" + pl
    extra = struct.pack(">II", 0, 0) + b"\x03lay"
    for key, payload in blocks + ([(b"iOpa", bytes([fill, 0, 0, 0]))] if fill != 255 else []):
        if len(payload) % 2:
            payload += b"\0"
        extra += b"8BIM" + key + struct.pack(">I", len(payload)) + payload
    rec = (struct.pack(">iiii", top, left, bottom, right) + struct.pack(">H", 4) + chans
           + b"8BIM" + b"norm" + bytes([255, 0, 0, 0]) + struct.pack(">I", len(extra)) + extra)
    body = struct.pack(">h", -1) + rec + data
    if len(body) % 2:
        body += b"\0"
    lm = struct.pack(">I", len(body)) + body + struct.pack(">I", 0)
    hdr = b"8BPS" + struct.pack(">H6xHIIHH", 1, 4, h, w, 8, psdparse.COLOR_MODE_RGB)
    psd = (hdr + struct.pack(">I", 0) + struct.pack(">I", 0) + struct.pack(">I", len(lm)) + lm
           + b"\0\0" + bytes(w * h * 4))
    path = tmp_path / "fx.psd"
    path.write_bytes(psd)
    p = psdparse.PSDFile()
    assert p.load(str(path))
    return p


def composite(p, effects=True):
    h = p.header
    out, stats = p.composite(effects=effects)
    return np.frombuffer(out, np.uint8).reshape(h.height, h.width, 4), stats


def test_outside_stroke(tmp_path):
    p = build(tmp_path, 40, 40, (10, 10, 30, 30), (255, 0, 0), [(b"lfx2", lfx2([stroke(3, (0, 0, 255))]))])
    img, _ = composite(p)
    assert tuple(img[20, 20]) == (0, 0, 255, 255)        # 中は赤 (BGRA)
    assert tuple(img[20, 8]) == (255, 0, 0, 255)         # 左に 3px の青い線
    assert img[20, 6, 3] == 0                            # その外は透明
    off, _ = composite(p, effects=False)
    assert off[20, 8, 3] == 0                            # 効果を切れば線は無い


def test_drop_shadow_direction(tmp_path):
    # 光源 90 度 (上) → 影は下へ
    p = build(tmp_path, 40, 40, (10, 10, 20, 20), (255, 255, 255),
              [(b"lfx2", lfx2([drop_shadow(90, 6, 0, (0, 0, 0))]))])
    img, _ = composite(p)
    assert img[24, 15, 3] == 255 and tuple(img[24, 15, :3]) == (0, 0, 0)   # 下に影
    assert img[6, 15, 3] == 0                                              # 上には無い


def test_color_overlay_survives_zero_fill(tmp_path):
    p = build(tmp_path, 20, 20, (5, 5, 15, 15), (255, 0, 0),
              [(b"lfx2", lfx2([color_overlay((0, 255, 0))]))], fill=0)
    img, _ = composite(p)
    assert tuple(img[10, 10]) == (0, 255, 0, 255)        # 塗り 0% でもオーバーレイは残る


def test_render_layer_includes_effect_margin(tmp_path):
    p = build(tmp_path, 40, 40, (10, 10, 30, 30), (255, 0, 0), [(b"lfx2", lfx2([stroke(3, (0, 0, 255))]))])
    bgra, left, top, w, h = p.render_layer(0)
    assert left < 10 - 3 and top < 10 - 3 and w > 26 and h > 26
    img = np.frombuffer(bgra, np.uint8).reshape(h, w, 4)
    assert tuple(img[20 - top, 8 - left]) == (255, 0, 0, 255)
    assert p.render_layer(0, effects=False)[1:] == (10, 10, 20, 20)
