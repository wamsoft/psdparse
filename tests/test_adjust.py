"""調整レイヤのパラメータ (layer.adjustment)。

サンプルは tests/data/make_adjust.py が psd-tools で作った adjustsample.psd。
レイヤ名 = 調整の種別で、各レイヤに既知の値を入れてある。
"""
import pytest

import psdparse
from conftest import DATA


@pytest.fixture(scope="module")
def adj():
    path = DATA / "adjustsample.psd"
    if not path.is_file():
        pytest.skip("adjustsample.psd not generated (run tests/data/make_adjust.py)")
    p = psdparse.PSDFile()
    assert p.load(str(path))
    out = {}
    for l in p.layers:
        out[l.name] = (l.layer_type, l.adjustment)
    return out


def _a(adj, name):
    layer_type, a = adj[name]
    assert layer_type == psdparse.LayerType.ADJUST
    assert a["type"] == name
    return a


def test_plain_layer_has_none(adj):
    assert adj["base"][1] is None


def test_levels(adj):
    a = _a(adj, "levels")
    assert a["key"] == "levl"
    assert len(a["records"]) == 29
    assert a["records"][0] == [10, 240, 0, 255, 1.5]   # 入力黒/白, 出力黒/白, ガンマ
    assert a["records"][1] == [0, 255, 0, 255, 1]


def test_curves(adj):
    a = _a(adj, "curves")
    assert a["channels"] == [0, 1]
    assert a["points"] == [[(0, 0), (128, 140), (255, 255)], [(0, 20), (255, 255)]]


def test_hue_saturation(adj):
    a = _a(adj, "hue_saturation")
    assert a["colorize"] == 0
    assert a["colorization"] == [0, 25, 0]
    assert a["master"] == [30, -10, 5]
    assert a["ranges"][0] == [315, 345, 15, 45, 10, -20, 5]


def test_simple_ones(adj):
    a = _a(adj, "brightness_contrast")
    assert (a["brightness"], a["contrast"], a["mean"], a["lab_only"]) == (-40, 25, 127, 0)
    a = _a(adj, "color_balance")
    assert (a["shadows"], a["midtones"], a["highlights"]) == ([10, 0, -5], [0, 20, 0], [-3, 0, 7])
    assert a["preserve_luminosity"] == 1
    a = _a(adj, "selective_color")
    assert a["method"] == 1 and a["records"][1] == [10, -20, 30, -40]
    assert _a(adj, "threshold")["level"] == 99
    assert _a(adj, "posterize")["levels"] == 6
    assert _a(adj, "invert")["key"] == "nvrt"


def test_channel_mixer_photo_filter_exposure(adj):
    a = _a(adj, "channel_mixer")
    assert a["monochrome"] == 0
    assert a["channels"] == [[100, 0, 0, 0, 0], [0, 100, 0, 0, 0], [0, 0, 100, 0, 0]]
    a = _a(adj, "photo_filter")
    assert (a["color_space"], a["color"], a["density"], a["preserve_luminosity"]) == (0, [65535, 32768, 0, 0], 25, 1)
    a = _a(adj, "exposure")
    assert (a["exposure"], a["offset"], a["gamma"]) == (0.5, -0.25, 1.5)


def test_roundtrip_unchanged(tmp_path):
    path = DATA / "adjustsample.psd"
    if not path.is_file():
        pytest.skip("adjustsample.psd not generated")
    p = psdparse.PSDFile()
    assert p.load(str(path))
    dst = tmp_path / "out.psd"
    assert p.save(str(dst))
    assert dst.read_bytes() == path.read_bytes()
