"""文書レベルの残り: スライス v8 (descriptor 形式)、注釈 ('Anno')、アートボード。

どれも小さな PSD をその場で組み立てて読む (アートボードは psd-tools で
masktest.psd のレイヤにブロックを足す)。
"""
import struct

import pytest

import psdparse


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


def text(s): return b"TEXT" + u(s + "\0")
def long(v): return b"long" + struct.pack(">i", v)
def enum(t, e): return b"enum" + cid(t) + cid(e)
def objc(c, items): return b"Objc" + desc(c, items)
def rect(l, t, r, b): return objc("Rct1", [("Top ", long(t)), ("Left", long(l)), ("Btom", long(b)), ("Rght", long(r))])


def _psd(resources=b"", layer_mask=b"", w=8, h=4):
    hdr = b"8BPS" + struct.pack(">H6xHIIHH", 1, 1, h, w, 8, psdparse.COLOR_MODE_GRAYSCALE)
    return (hdr + struct.pack(">I", 0) + struct.pack(">I", len(resources)) + resources
            + struct.pack(">I", len(layer_mask)) + layer_mask + b"\0\0" + bytes(w * h))


def _load(tmp_path, data):
    path = tmp_path / "in.psd"
    path.write_bytes(data)
    p = psdparse.PSDFile()
    assert p.load(str(path))
    dst = tmp_path / "out.psd"
    assert p.save(str(dst))
    assert dst.read_bytes() == data          # 未編集なら元どおり
    return p


def test_slices_v8(tmp_path):
    sl = objc("slice", [
        ("sliceID", long(3)), ("groupID", long(1)),
        ("origin", enum("ESliceOrigin", "userGenerated")),
        ("Nm  ", text("btn")),
        ("Type", enum("ESliceType", "Img ")),
        ("bounds", rect(1, 2, 5, 4)),
        ("url", text("http://example.com")), ("null", text("_blank")),
        ("Msge", text("hi")), ("altTag", text("alt")),
        ("cellTextIsHTML", b"bool\x01"), ("cellText", text("")),
        ("horzAlign", enum("ESliceHorzAlign", "default")),
        ("vertAlign", enum("ESliceVertAlign", "default")),
    ])
    d = desc("null", [("baseName", text("Doc")), ("bounds", rect(0, 0, 8, 4)),
                      ("slices", b"VlLs" + struct.pack(">I", 1) + sl)])
    data = struct.pack(">II", 8, 16) + d
    res = b"8BIM" + struct.pack(">H", 1050) + b"\0\0" + struct.pack(">I", len(data)) + data
    if len(data) % 2:
        res += b"\0"
    p = _load(tmp_path, _psd(resources=res))
    s = p.slices
    assert s["version"] == 8
    assert s["group_name"] == "Doc"
    assert s["bounding"] == {"left": 0, "top": 0, "right": 8, "bottom": 4}
    (one,) = s["slices"]
    assert (one["id"], one["group_id"], one["origin"], one["type"]) == (3, 1, 2, 1)
    assert (one["left"], one["top"], one["right"], one["bottom"]) == (1, 2, 5, 4)
    assert (one["name"], one["url"], one["target"], one["message"], one["alt_tag"]) == \
        ("btn", "http://example.com", "_blank", "hi", "alt")


def _pascal_even(b):
    out = bytes([len(b)]) + b
    return out + (b"\0" if len(out) % 2 else b"")


def test_annotations(tmp_path):
    body = "メモ\r二行目".encode("utf-16-be")
    payload = b"\xfe\xff" + body
    dblock = b"txtC" + struct.pack(">I", len(payload)) + payload
    dblock = struct.pack(">I", len(dblock) + 4) + dblock
    note = (b"txtA" + b"\x01" + b"\x00" + struct.pack(">H", 1)
            + struct.pack(">4i", 1, 2, 3, 4) + struct.pack(">4i", 10, 20, 110, 220)
            + struct.pack(">H4H", 0, 65535, 65535, 0, 0)
            + _pascal_even(b"me") + _pascal_even(b"note") + _pascal_even(b"D:2026")
            + dblock)
    note = struct.pack(">I", len(note) + 4) + note
    anno = struct.pack(">HHI", 2, 1, 1) + note
    block = b"8BIMAnno" + struct.pack(">I", len(anno)) + anno + b"\0" * ((4 - len(anno) % 4) % 4)
    layer_mask = struct.pack(">I", 0) + struct.pack(">I", 0) + block
    p = _load(tmp_path, _psd(layer_mask=layer_mask))
    (a,) = p.annotations
    assert a["kind"] == "text" and a["open"] is True
    assert a["text"] == "メモ\r二行目"
    assert a["popup_rect"] == (10, 20, 110, 220)
    assert (a["author"], a["name"], a["mod_date"]) == (b"me", b"note", b"D:2026")
    assert a["color"] == (65535, 65535, 0, 0)


def test_artboard(sample_mask_psd, tmp_path):
    psd_tools = pytest.importorskip("psd_tools")
    from psd_tools.psd.tagged_blocks import TaggedBlock

    def dbl(v): return b"doub" + struct.pack(">d", v)
    d = desc("artboard", [
        ("artboardRect", objc("classFloatRect", [("Top ", dbl(10)), ("Left", dbl(20)),
                                                 ("Btom", dbl(110)), ("Rght", dbl(220))])),
        ("artboardPresetName", text("iPhone")),
        ("Clr ", objc("RGBC", [("Rd  ", dbl(255)), ("Grn ", dbl(128)), ("Bl  ", dbl(0))])),
        ("artboardBackgroundType", long(4)),
    ])
    t = psd_tools.PSDImage.open(sample_mask_psd)
    rec = t._record
    r = rec.layer_and_mask_information.layer_info.layer_records[0]
    r.tagged_blocks[b"artb"] = TaggedBlock(key=b"artb", data=struct.pack(">I", 16) + d)
    dst = tmp_path / "artb.psd"
    with open(dst, "wb") as f:
        rec.write(f)
    p = psdparse.PSDFile()
    assert p.load(str(dst))
    a = p.layers[0].artboard
    assert a["rect"] == (20.0, 10.0, 220.0, 110.0)
    assert a["preset_name"] == "iPhone"
    assert a["background_type"] == 4
    assert a["color"] == (255.0, 128.0, 0.0)
