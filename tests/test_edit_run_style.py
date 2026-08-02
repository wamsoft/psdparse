"""Tests for per-run text style editing (E6 styling, 0.7.0):

set_run_style edits the StyleSheetData values of an existing style run in the
embedded EngineData (font size / colour / tracking / kerning / bold / italic /
underline), leaving the text, run lengths and other runs untouched.
"""
import pytest

import psdparse


def _multirun_layer(p):
    idx = next((i for i, l in enumerate(p.layers)
                if l.text is not None and len(l.text["runs"]) >= 2), None)
    if idx is None:
        pytest.skip("sample has no multi-run text layer")
    return idx


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def test_set_run_size_color_tracking(psd_text, tmp_path):
    i = _multirun_layer(psd_text)
    text_before = psd_text.layers[i].text["text"]
    run1_before = dict(psd_text.layers[i].text["runs"][1])

    psd_text.set_run_style(i, 0, size_px=40.0, color=(1.0, 0.0, 0.0), tracking=50)
    q = _reload(psd_text, tmp_path)
    r = q.layers[i].text["runs"]
    assert r[0]["size_px"] == pytest.approx(40.0)
    assert tuple(round(c, 3) for c in r[0]["color"][:3]) == (1.0, 0.0, 0.0)
    assert r[0]["tracking"] == 50
    # text + other run untouched
    assert q.layers[i].text["text"] == text_before
    assert r[1]["size_px"] == run1_before["size_px"]
    assert r[1]["color"] == run1_before["color"]


def test_set_run_color_with_alpha(psd_text, tmp_path):
    i = _multirun_layer(psd_text)
    psd_text.set_run_style(i, 1, color=(0.0, 1.0, 0.0, 0.5))
    q = _reload(psd_text, tmp_path)
    c = q.layers[i].text["runs"][1]["color"]
    assert tuple(round(x, 3) for x in c) == (0.0, 1.0, 0.0, 0.5)


def test_set_run_style_run_count_unchanged(psd_text, tmp_path):
    i = _multirun_layer(psd_text)
    n = len(psd_text.layers[i].text["runs"])
    psd_text.set_run_style(i, 0, size_px=22.0)
    q = _reload(psd_text, tmp_path)
    assert len(q.layers[i].text["runs"]) == n


def test_set_run_style_out_of_range_raises(psd_text):
    i = _multirun_layer(psd_text)
    with pytest.raises(RuntimeError):
        psd_text.set_run_style(i, 999, size_px=10.0)


def test_set_run_style_no_args_raises(psd_text):
    i = _multirun_layer(psd_text)
    with pytest.raises(ValueError):
        psd_text.set_run_style(i, 0)


def test_set_run_style_non_text_raises(psd_group):
    idx = next((i for i, l in enumerate(psd_group.layers) if l.text is None), None)
    if idx is None:
        pytest.skip("all layers are text")
    with pytest.raises(RuntimeError):
        psd_group.set_run_style(idx, 0, size_px=10.0)


def test_run_style_valid_for_psd_tools(psd_text, tmp_path):
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    i = _multirun_layer(psd_text)
    psd_text.set_run_style(i, 0, size_px=33.0, color=(1.0, 0.0, 0.0))
    dst = tmp_path / "rs.psd"
    assert psd_text.save(str(dst))
    PSDImage.open(str(dst)).composite()   # full decode must not raise
