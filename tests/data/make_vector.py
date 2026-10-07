"""ベクタマスクとパス (保存パス / 作業パス / Unicode 名) のサンプルを psd-tools で作る。

  python make_vector.py <出力先.psd>

作るもの (100x50 の RGB):
  layer 0 "rect"   赤のレイヤに vmsk。閉じた矩形 (20,10)-(60,40) の 1 本。
                   反転フラグ付き。knot はすべて連動 (制御点 = アンカー)
  layer 1 "tri"    緑のレイヤに vmsk。閉じた三角形 + 前面の型抜き (operation 2)
                   の矩形。三角形の頂点 (50,5) は制御点が左右に開いた非連動 knot
  保存パス 2000    リソース名 "path1" (ASCII)、開いたパス 2 点
  保存パス 2001    リソース名は Shift-JIS の「パス」、pths に Unicode 名「パス２」
  作業パス 1025    閉じた矩形 1 本
"""
import io
import struct
import sys
from pathlib import Path as FsPath

from PIL import Image
from psd_tools import PSDImage
from psd_tools.api.layers import PixelLayer
from psd_tools.constants import Tag
from psd_tools.psd.image_resources import ImageResource
from psd_tools.psd.tagged_blocks import TaggedBlock
from psd_tools.psd.vector import (
    ClosedKnotLinked, ClosedKnotUnlinked, ClosedPath, InitialFillRule, OpenKnotLinked,
    OpenPath, Path, PathFillRule, VectorMaskSetting,
)

W, H = 100, 50


def pt(x, y):
    """ピクセル → psd-tools の (縦, 横) 比率"""
    return (y / H, x / W)


def linked(x, y, cls=ClosedKnotLinked):
    return cls(preceding=pt(x, y), anchor=pt(x, y), leaving=pt(x, y))


def rect(l, t, r, b, operation=1, index=0):
    knots = [linked(l, t), linked(r, t), linked(r, b), linked(l, b)]
    return ClosedPath(items=knots, operation=operation, index=index)


def path_bytes(path):
    buf = io.BytesIO()
    path.write(buf)
    return buf.getvalue()


def desc_unicode(s):
    u = s.encode("utf-16-be")
    return struct.pack(">I", len(u) // 2) + u


def desc_id(s):
    b = s.encode("ascii")
    return struct.pack(">I", 0 if len(b) == 4 else len(b)) + b


def pths_block(names):
    """'pths': version 16 + descriptor { pathList: [ { pathUnicodeName } ] }"""
    items = b""
    for n in names:
        items += (b"Objc" + desc_unicode("\0") + desc_id("pathClass") + struct.pack(">I", 1)
                  + desc_id("pathUnicodeName") + b"TEXT" + desc_unicode(n + "\0"))
    body = (desc_unicode("\0") + desc_id("null") + struct.pack(">I", 1)
            + desc_id("pathList") + b"VlLs" + struct.pack(">I", len(names)) + items)
    return struct.pack(">I", 16) + body


def main(out):
    psd = PSDImage.new("RGB", (W, H))
    PixelLayer.frompil(Image.new("RGB", (W, H), (255, 0, 0)), psd, "rect")
    PixelLayer.frompil(Image.new("RGB", (W, H), (0, 255, 0)), psd, "tri")
    psd.save(out)

    psd = PSDImage.open(out)
    rec = psd._record
    records = rec.layer_and_mask_information.layer_info.layer_records

    mask0 = Path(items=[PathFillRule(), InitialFillRule(value=0), rect(20, 10, 60, 40)])
    records[0].tagged_blocks[Tag.VECTOR_MASK_SETTING1] = TaggedBlock(
        key=Tag.VECTOR_MASK_SETTING1, data=VectorMaskSetting(version=3, flags=1, path=mask0))

    apex = ClosedKnotUnlinked(preceding=pt(45, 5), anchor=pt(50, 5), leaving=pt(55, 5))
    tri = ClosedPath(items=[apex, linked(90, 45), linked(10, 45)], operation=1, index=0)
    hole = rect(40, 30, 60, 40, operation=2, index=1)
    mask1 = Path(items=[PathFillRule(), InitialFillRule(value=0), tri, hole])
    records[1].tagged_blocks[Tag.VECTOR_MASK_SETTING1] = TaggedBlock(
        key=Tag.VECTOR_MASK_SETTING1, data=VectorMaskSetting(version=3, flags=0, path=mask1))

    open_path = OpenPath(items=[linked(5, 5, OpenKnotLinked), linked(95, 45, OpenKnotLinked)],
                         operation=-1, index=0)
    res = rec.image_resources
    res[2000] = ImageResource(key=2000, name="path1",
                              data=path_bytes(Path(items=[PathFillRule(), InitialFillRule(value=0), open_path])))
    res[2001] = ImageResource(key=2001, name="XXXX",   # psd-tools は MacRoman でしか書けないので後で差し替え
                              data=path_bytes(Path(items=[PathFillRule(), InitialFillRule(value=0), rect(0, 0, 10, 10)])))
    res[1025] = ImageResource(key=1025, name="",
                              data=path_bytes(Path(items=[PathFillRule(), InitialFillRule(value=1), rect(30, 20, 70, 30)])))

    lm = rec.layer_and_mask_information
    lm.tagged_blocks[b"pths"] = TaggedBlock(key=b"pths", data=pths_block(["path1", "パス２"]))
    with open(out, "wb") as f:
        rec.write(f)

    # 2001 のリソース名を Shift-JIS の生バイトへ (同じ長さなので位置はずれない)
    data = bytearray(FsPath(out).read_bytes())
    sjis = "パス".encode("cp932")
    i = data.find(b"8BIM\x07\xd1")
    assert i > 0 and data[i + 6] == len(sjis)
    data[i + 7:i + 7 + len(sjis)] = sjis
    FsPath(out).write_bytes(bytes(data))

    t = PSDImage.open(out)
    print([(l.name, l.has_vector_mask()) for l in t],
          [(int(k), v.name) for k, v in t._record.image_resources.items() if int(k) in (1025, 2000, 2001)])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else str(FsPath(__file__).with_name("vectorsample.psd")))
