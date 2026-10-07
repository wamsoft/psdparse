"""PSB と 16/32bit レイヤ (Lr16 / Lr32) のサンプルを psd-tools で作る。

Photoshop は 16/32bit 文書のレイヤを通常の layer info ではなく、文書末尾の
追加情報 'Lr16' / 'Lr32' に置く (layer info 本体は空)。psd-tools は保存時に
通常の layer info へ書くので、低レベル API で Lr16 / Lr32 へ移し替えて
Photoshop と同じ形にする。

psd-tools の癖への対処:
  * PSB の RLE 行長 (4 バイト) はレイヤ追加時の版数で決まるので、版数を
    先に 2 にしてからレイヤを足す。
  * 32bit 文書でも PIL 画像を 8bit のまま書いてしまうので、チャンネルを
    float32 (big-endian) で書き直す。

  python make_highdepth.py <出力先フォルダ>

作るファイル (どれも 32x24、RGB):
  psbsample.psb   8bit の PSB (version 2)。赤 (3,2)-(13,10) / 緑 (8,6)-(20,18)
  lr16sample.psd  16bit。レイヤは Lr16。赤 / 緑 (同上)
  lr32sample.psd  32bit。レイヤは Lr32。赤 1 枚
  lr16sample.psb  16bit の PSB。レイヤは Lr16 (8 byte 長キー)
"""
import struct
import sys
from pathlib import Path

from PIL import Image
from psd_tools import PSDImage
from psd_tools.api.layers import PixelLayer
from psd_tools.constants import Tag
from psd_tools.psd.layer_and_mask import LayerInfo, LayerInfoBlock
from psd_tools.psd.tagged_blocks import TaggedBlock, TaggedBlocks

RED = ("red", (255, 0, 0, 255), 3, 2, 10, 8)
GREEN = ("green", (0, 255, 0, 255), 8, 6, 12, 12)


def build(depth, layers, version=1):
    psd = PSDImage.new("RGB", (32, 24), depth=depth)
    psd._record.header.version = version
    for name, color, left, top, w, h in layers:
        im = Image.new("RGBA", (w, h), color)
        PixelLayer.frompil(im, psd, name, top=top, left=left)
    return psd


def fix_float32(path, layers):
    """32bit 文書のチャンネルを float32 で書き直す"""
    psd = PSDImage.open(path)
    rec = psd._record
    info = rec.layer_and_mask_information.layer_info
    for record, channels, spec in zip(info.layer_records, info.channel_image_data, layers):
        _, color, _, _, w, h = spec
        for ci, cd in zip(record.channel_info, channels):
            if ci.id == -2:   # マスク (frompil が alpha から作る。文書と同じ深度)
                mw = record.mask_data.right - record.mask_data.left
                mh = record.mask_data.bottom - record.mask_data.top
                cd.set_data(struct.pack(">f", 1.0) * (mw * mh), mw, mh, 32, rec.header.version)
                ci.length = len(cd.data) + 2
                continue
            v = 1.0 if ci.id == -1 else color[ci.id] / 255.0
            cd.set_data(struct.pack(">f", v) * (w * h), w, h, 32, rec.header.version)
            ci.length = len(cd.data) + 2
    with open(path, "wb") as f:
        rec.write(f)


def move_to_tagged(path, key):
    """保存済みファイルの layer info を末尾の key ブロックへ移す"""
    psd = PSDImage.open(path)
    rec = psd._record
    lm = rec.layer_and_mask_information
    info = lm.layer_info
    block = LayerInfoBlock(
        layer_count=info.layer_count,
        layer_records=info.layer_records,
        channel_image_data=info.channel_image_data,
    )
    if lm.tagged_blocks is None:
        lm.tagged_blocks = TaggedBlocks()
    lm.tagged_blocks[key] = TaggedBlock(key=key, data=block)
    lm.layer_info = LayerInfo()
    with open(path, "wb") as f:
        rec.write(f)


def main(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)

    build(8, [RED, GREEN], version=2).save(out / "psbsample.psb")

    build(16, [RED, GREEN]).save(out / "lr16sample.psd")
    move_to_tagged(out / "lr16sample.psd", Tag.LAYER_16)

    build(32, [RED]).save(out / "lr32sample.psd")
    fix_float32(out / "lr32sample.psd", [RED])
    move_to_tagged(out / "lr32sample.psd", Tag.LAYER_32)

    build(16, [RED, GREEN], version=2).save(out / "lr16sample.psb")
    move_to_tagged(out / "lr16sample.psb", Tag.LAYER_16)

    for name in ("psbsample.psb", "lr16sample.psd", "lr32sample.psd", "lr16sample.psb"):
        p = PSDImage.open(out / name)
        print(name, p.version, p.depth,
              [(l.name, l.bbox, tuple(l.numpy()[0, 0])) for l in p])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parent)
