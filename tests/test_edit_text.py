"""Tests for text-layer editing (E6, 0.7.0):

A byte-exact Adobe *EngineData* serializer (the inverse of the psdengine parser,
matching Photoshop / psd-tools formatting) underpins set_text. Editing rewrites
the embedded EngineData body + run lengths and the 'Txt ' descriptor string,
then re-serializes the TySh block (descriptor serializer) with the warp/bounds
suffix preserved.
"""
import pytest

import psdparse


def _text_layers(p):
    idx = [i for i, l in enumerate(p.layers) if l.text is not None]
    if not idx:
        pytest.skip("sample has no text layers")
    return idx


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def test_engine_data_serializer_byte_exact(psd_text):
    # Re-serializing each layer's EngineData must reproduce the exact bytes.
    for i in _text_layers(psd_text):
        d = psd_text.layers[i].descriptor("TySh", 56)
        if not d or "EngineData" not in d:
            continue
        eng = d["EngineData"]
        assert psdparse._reserialize_engine_data(eng) == eng


def test_set_text(psd_text, tmp_path):
    i = _text_layers(psd_text)[0]
    psd_text.set_text(i, "hello\rworld")
    q = _reload(psd_text, tmp_path)
    assert q.layers[i].text["text"].rstrip("\r") == "hello\rworld"


def test_set_text_unicode_emoji(psd_text, tmp_path):
    i = _text_layers(psd_text)[0]
    psd_text.set_text(i, "こんにちは🌏")
    q = _reload(psd_text, tmp_path)
    assert q.layers[i].text["text"].rstrip("\r") == "こんにちは🌏"


def test_set_text_leaves_other_layers(psd_text, tmp_path):
    idx = _text_layers(psd_text)
    if len(idx) < 2:
        pytest.skip("need >=2 text layers")
    i, j = idx[0], idx[1]
    before_j = psd_text.layers[j].text["text"]
    psd_text.set_text(i, "changed")
    q = _reload(psd_text, tmp_path)
    assert q.layers[j].text["text"] == before_j


def test_set_text_non_text_raises(psd_group):
    idx = next((i for i, l in enumerate(psd_group.layers) if l.text is None), None)
    if idx is None:
        pytest.skip("all layers are text")
    with pytest.raises(RuntimeError):
        psd_group.set_text(idx, "x")


def test_set_text_valid_for_psd_tools(psd_text, tmp_path):
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    i = _text_layers(psd_text)[0]
    psd_text.set_text(i, "PSDTOOLS-CHECK🌏")
    dst = tmp_path / "t.psd"
    assert psd_text.save(str(dst))
    pt = PSDImage.open(str(dst))
    pt.composite()
    texts = [l.text for l in pt.descendants() if l.kind == "type"]
    assert any("PSDTOOLS-CHECK🌏" in (t or "") for t in texts)
