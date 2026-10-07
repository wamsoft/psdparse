"""スマートオブジェクトのサンプルを psd-tools で作る。

  python make_smart.py <出力先.psd>

作るもの (64x48 の RGB):
  layer 0 "plain"  ふつうのレイヤ
  layer 1 "smart"  SoLd 付き。中身は埋め込みの PNG (8x6)。四隅の変形は
                   (10,5) (42,7) (40,30) (12,28)、元の大きさ 8x6
                   descriptor には 64bit 整数 ('comp')、単位付き実数の配列 ('UnFl')
                   を持つオブジェクト配列 ('ObAr')、長さ 4 を明示した 4 文字の
                   キー ('warp') を入れてある (書き戻しの byte 一致を見るため)
  文書末尾 lnk2    item 0: 埋め込み (liFD) の PNG "tile.png"
                   item 1: エイリアス (liFA) "missing.psd"、中身なし

psd-tools は lnk2 / SoLd を生バイトのまま書けるので、中身は自前で組み立てる。
"""
import io
import struct
import sys
from pathlib import Path

from PIL import Image
from psd_tools import PSDImage
from psd_tools.api.layers import PixelLayer
from psd_tools.constants import Tag
from psd_tools.psd.tagged_blocks import TaggedBlock

UUID = "8a6ef7c2-1d3b-4c55-9e0f-0123456789ab"
CORNERS = [10, 5, 42, 7, 40, 30, 12, 28]


def u(s):
    """descriptor の Unicode 文字列 (文字数 + UTF-16BE)"""
    b = s.encode("utf-16-be")
    return struct.pack(">I", len(b) // 2) + b


def cid(s, explicit=False):
    """descriptor の ID。4 文字は長さ 0、ただし explicit なら長さ 4 を明示"""
    b = s.encode("ascii")
    n = len(b) if (len(b) != 4 or explicit) else 0
    return struct.pack(">I", n) + b


def desc(class_id, items):
    body = u("\0") + cid(class_id) + struct.pack(">I", len(items))
    for key, value in items:
        body += (cid(key[0], True) if isinstance(key, tuple) else cid(key)) + value
    return body


def text(s): return b"TEXT" + u(s + "\0")
def long(v): return b"long" + struct.pack(">i", v)
def doub(v): return b"doub" + struct.pack(">d", v)
def comp(v): return b"comp" + struct.pack(">q", v)
def objc(class_id, items): return b"Objc" + desc(class_id, items)
def vlls(values): return b"VlLs" + struct.pack(">I", len(values)) + b"".join(values)
def unfl(unit, values): return b"UnFl" + unit + struct.pack(">I", len(values)) + b"".join(struct.pack(">d", v) for v in values)


def sold_block():
    mesh = (b"ObAr" + struct.pack(">I", 4)
            + desc("rationalPoint", [("Hrzn", unfl(b"#Pxl", [0, 8, 0, 8])),
                                     ("Vrtc", unfl(b"#Pxl", [0, 0, 6, 6]))]))
    items = [
        ("Idnt", text(UUID)),
        ("placed", text("instance-1")),
        ("PgNm", long(1)),
        ("totalPages", long(1)),
        ("Annt", long(16)),
        ("Type", long(2)),
        ("Trnf", vlls([doub(v) for v in CORNERS])),
        ("Sz  ", objc("Pnt ", [("Wdth", doub(8.0)), ("Hght", doub(6.0))])),
        ("comp", comp(-1)),
        (("warp",), objc("warp", [("meshPoints", mesh)])),
    ]
    return b"soLD" + struct.pack(">II", 4, 16) + desc("null", items)


def linked_item(kind, name, filetype, data):
    item = kind + struct.pack(">I", 7)
    uid = UUID.encode("ascii") if kind == b"liFD" else b"alias-uuid"
    item += bytes([len(uid)]) + uid + u(name + "\0") + filetype + b"8BIM"
    item += struct.pack(">Q", len(data)) + b"\0"
    if kind == b"liFA":
        item += b"\0" * 8
    item += data
    item += u("") + struct.pack(">d", 0.0) + b"\0"     # child id / 更新時刻 / ロック
    out = struct.pack(">Q", len(item)) + item
    return out + b"\0" * ((4 - len(item) % 4) % 4)


def main(out):
    png = io.BytesIO()
    Image.new("RGB", (8, 6), (0, 0, 255)).save(png, "PNG")
    png = png.getvalue()

    psd = PSDImage.new("RGB", (64, 48))
    PixelLayer.frompil(Image.new("RGB", (64, 48), (255, 255, 255)), psd, "plain")
    PixelLayer.frompil(Image.new("RGB", (32, 25), (0, 0, 255)), psd, "smart", top=5, left=10)
    psd.save(out)

    psd = PSDImage.open(out)
    rec = psd._record
    records = rec.layer_and_mask_information.layer_info.layer_records
    records[1].tagged_blocks[b"SoLd"] = TaggedBlock(key=b"SoLd", data=sold_block())
    lnk = linked_item(b"liFD", "tile.png", b"png ", png) + linked_item(b"liFA", "missing.psd", b"8BPS", b"")
    rec.layer_and_mask_information.tagged_blocks[b"lnk2"] = TaggedBlock(key=b"lnk2", data=lnk)
    with open(out, "wb") as f:
        rec.write(f)

    t = PSDImage.open(out)
    layers = list(t)
    print([(l.name, l.kind) for l in layers])
    so = layers[1]._record.tagged_blocks.get_data(Tag.SMART_OBJECT_LAYER_DATA1)
    print(so.data.get(b"Idnt"), list(so.data.get(b"Trnf")))
    linked = t._record.layer_and_mask_information.tagged_blocks.get_data(Tag.LINKED_LAYER2)
    print([(x.kind, x.filename, len(x.data or b"")) for x in linked])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).with_name("smartsample.psd")))
