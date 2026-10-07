"""調整レイヤのサンプルを psd-tools で作る。

  python make_adjust.py <出力先.psd>

32x32 の RGB に、1 レイヤ 1 種類ずつ調整ブロックを付ける (レイヤ名 = 種別)。
値は test_adjust.py が確かめる既知の値。
"""
import struct
import sys
from pathlib import Path

from PIL import Image
from psd_tools import PSDImage
from psd_tools.api.layers import PixelLayer
from psd_tools.constants import Tag
from psd_tools.psd.adjustments import (
    BrightnessContrast, ChannelMixer, ColorBalance, Curves, Exposure, HueSaturation,
    LevelRecord, Levels, PhotoFilter, SelectiveColor,
)
from psd_tools.psd.tagged_blocks import TaggedBlock


def levels():
    recs = [LevelRecord(10, 240, 0, 255, 150)] + [LevelRecord(0, 255, 0, 255, 100)] * 28
    return Levels(version=2, items=recs)


def curves():
    # version 4: チャンネル数ぶん (点の数 + (出力, 入力) の組)
    return Curves(is_map=False, version=4, count_map=2,
                  data=[[(0, 0), (140, 128), (255, 255)], [(20, 0), (255, 255)]])


def hue_sat():
    ranges = [((315, 345, 15, 45), (10, -20, 5))] + [((0, 0, 0, 0), (0, 0, 0))] * 5
    return HueSaturation(version=2, enable=0, colorization=(0, 25, 0), master=(30, -10, 5), items=ranges)


def u16(v):
    return v & 0xFFFF


BLOCKS = [
    ("levels", Tag.LEVELS, levels),
    ("curves", Tag.CURVES, curves),
    ("hue_saturation", Tag.HUE_SATURATION, hue_sat),
    ("brightness_contrast", Tag.BRIGHTNESS_AND_CONTRAST,
     lambda: BrightnessContrast(brightness=u16(-40), contrast=25, mean=127, lab_only=0)),
    ("color_balance", Tag.COLOR_BALANCE,
     lambda: ColorBalance(shadows=(10, 0, -5), midtones=(0, 20, 0), highlights=(-3, 0, 7), luminosity=True)),
    ("selective_color", Tag.SELECTIVE_COLOR,
     lambda: SelectiveColor(version=1, method=1, data=[(0, 0, 0, 0), (10, -20, 30, -40)] + [(0, 0, 0, 0)] * 8)),
    ("threshold", Tag.THRESHOLD, lambda: struct.pack(">HH", 99, 0)),
    ("posterize", Tag.POSTERIZE, lambda: struct.pack(">HH", 6, 0)),
    ("invert", Tag.INVERT, lambda: b""),
    ("channel_mixer", Tag.CHANNEL_MIXER,
     lambda: ChannelMixer(version=1, monochrome=0, data=[100, 0, 0, 0, 0],
                          unknown=struct.pack(">10h", 0, 100, 0, 0, 0, 0, 0, 100, 0, 0) + b"\0\0")),
    ("photo_filter", Tag.PHOTO_FILTER,
     lambda: PhotoFilter(version=2, color_space=0, color_components=(65535, 32768, 0, 0), density=25, luminosity=1)),
    ("exposure", Tag.EXPOSURE, lambda: Exposure(version=1, exposure=0.5, offset=-0.25, gamma=1.5)),
]


def main(out):
    psd = PSDImage.new("RGB", (32, 32))
    PixelLayer.frompil(Image.new("RGB", (32, 32), (128, 128, 128)), psd, "base")
    for name, _, _ in BLOCKS:
        PixelLayer.frompil(Image.new("RGB", (1, 1), (0, 0, 0)), psd, name)
    psd.save(out)

    psd = PSDImage.open(out)
    rec = psd._record
    records = rec.layer_and_mask_information.layer_info.layer_records
    for (name, tag, make), r in zip(BLOCKS, records[1:]):
        r.tagged_blocks[tag] = TaggedBlock(key=tag, data=make())
    with open(out, "wb") as f:
        rec.write(f)

    t = PSDImage.open(out)
    print([(l.name, l.kind) for l in t])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).with_name("adjustsample.psd")))
