"""スマートオブジェクト: 文書末尾の埋め込み / リンクファイル (lnk2 など) と、
レイヤ側の配置情報 (SoLd / SoLE / PlLd)。

サンプルは tests/data/make_smart.py が作った smartsample.psd。埋め込みの PNG と
エイリアス 1 件、SoLd 付きのレイヤ 1 枚。SoLd の descriptor には 64bit 整数
('comp')、単位付き実数の配列 ('UnFl') を持つオブジェクト配列 ('ObAr')、長さ 4 を
明示した 4 文字のキーが入っている。
"""
import io

import pytest

import psdparse
from conftest import DATA

UUID = "8a6ef7c2-1d3b-4c55-9e0f-0123456789ab"


@pytest.fixture()
def psd():
    path = DATA / "smartsample.psd"
    if not path.is_file():
        pytest.skip("smartsample.psd not generated (run tests/data/make_smart.py)")
    p = psdparse.PSDFile()
    assert p.load(str(path))
    return p


def test_linked_files(psd):
    data, alias = psd.linked_files
    assert data["kind"] == "data"
    assert data["uuid"] == UUID
    assert data["name"] == "tile.png"
    assert data["file_type"] == b"png "
    assert data["has_data"] is True
    assert data["block"] == "lnk2"
    assert alias["kind"] == "alias"
    assert alias["name"] == "missing.psd"
    assert alias["has_data"] is False


def test_linked_file_data(psd):
    png = psd.linked_file_data(0)
    assert png == psd.linked_file_data(UUID)          # 添字でも uuid でも引ける
    assert len(png) == psd.linked_files[0]["size"]
    Image = pytest.importorskip("PIL.Image")
    im = Image.open(io.BytesIO(png))
    assert im.size == (8, 6)
    assert im.convert("RGB").getpixel((0, 0)) == (0, 0, 255)
    assert psd.linked_file_data(1) is None             # エイリアスは中身なし
    with pytest.raises(IndexError):
        psd.linked_file_data(5)


def test_smart_object_layer(psd):
    assert psd.layers[0].smart_object is None
    so = psd.layers[1].smart_object
    assert so["key"] == "SoLd"
    assert so["uuid"] == UUID
    assert so["placed_id"] == "instance-1"
    assert so["placed_type"] == 2
    assert so["transform"] == [(10.0, 5.0), (42.0, 7.0), (40.0, 30.0), (12.0, 28.0)]
    assert so["size"] == (8.0, 6.0)
    assert so["filters"] is None
    assert so["linked_file"] == 0


def test_new_descriptor_types(psd):
    d = psd.layers[1].descriptor("SoLd")
    assert d["comp"] == -1
    mesh = d["warp"]["meshPoints"]
    assert mesh["Hrzn"] == {"values": [0.0, 8.0, 0.0, 8.0], "unit": "pixels"}
    assert mesh["Vrtc"]["values"] == [0.0, 0.0, 6.0, 6.0]


def test_descriptor_rewrite_is_byte_exact(psd, tmp_path):
    """変更なしで SoLd を書き戻しても 1 バイトも変わらない
    (新しい 3 型と、長さ 4 を明示した ID の書き方を再現できている)"""
    before = psd.layers[1].descriptor_bytes("SoLd")
    psd.set_layer_descriptor(1, "SoLd", {}, 12)
    assert psd.layers[1].descriptor_bytes("SoLd") == before
    dst = tmp_path / "out.psd"
    assert psd.save(str(dst))
    assert dst.read_bytes() == (DATA / "smartsample.psd").read_bytes()


def test_matches_psd_tools(psd):
    psd_tools = pytest.importorskip("psd_tools")
    from psd_tools.constants import Tag
    t = psd_tools.PSDImage.open(DATA / "smartsample.psd")
    linked = t._record.layer_and_mask_information.tagged_blocks.get_data(Tag.LINKED_LAYER2)
    assert [x.filename.rstrip("\0") for x in linked] == [f["name"] for f in psd.linked_files]
    assert linked[0].data == psd.linked_file_data(0)
