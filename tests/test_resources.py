"""Tests for image-resource raw-bytes access (0.5.0):

PSDFile.image_resource_ids / image_resource(id) and the typed shortcuts
icc_profile / exif / xmp / thumbnail. EXIF and ICC bytes here were verified
byte-identical against psd-tools 1.17.
"""
import psdparse
import pytest


def test_resource_ids_are_ints(psd_group):
    ids = psd_group.image_resource_ids
    assert isinstance(ids, list)
    assert all(isinstance(i, int) for i in ids)
    assert len(ids) > 0


def test_generic_matches_typed(psd_group):
    # image_resource(1058) is the same bytes as the exif shortcut.
    assert psd_group.image_resource(1058) == psd_group.exif


def test_absent_resource_is_none(psd_group):
    # 9999 is not a real Photoshop resource id.
    assert psd_group.image_resource(9999) is None


def test_exif_present(psd_group):
    exif = psd_group.exif
    if exif is None:
        pytest.skip("sample has no EXIF resource")
    assert isinstance(exif, bytes) and len(exif) > 0


def test_xmp_is_xml(psd_group):
    xmp = psd_group.xmp
    if xmp is None:
        pytest.skip("sample has no XMP resource")
    assert isinstance(xmp, str)
    assert "xpacket" in xmp or "xmpmeta" in xmp


def test_icc_profile(psd_text):
    # fontsample.psd carries an ICC profile.
    icc = psd_text.icc_profile
    if icc is None:
        pytest.skip("sample has no ICC profile")
    assert isinstance(icc, bytes) and len(icc) > 0


def test_thumbnail_header(psd_group):
    th = psd_group.thumbnail
    if th is None:
        pytest.skip("sample has no thumbnail")
    assert th["width"] > 0 and th["height"] > 0
    assert th["format"] in ("jpeg", "raw")
    assert isinstance(th["data"], bytes)
    if th["format"] == "jpeg":
        assert th["data"][:2] == b"\xff\xd8"  # JFIF/JPEG SOI marker


def test_thumbnail_decodes(psd_group):
    Image = pytest.importorskip("PIL.Image")
    th = psd_group.thumbnail
    if th is None or th["format"] != "jpeg":
        pytest.skip("sample has no JPEG thumbnail")
    import io
    img = Image.open(io.BytesIO(th["data"]))
    assert (img.width, img.height) == (th["width"], th["height"])
