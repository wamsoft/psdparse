"""Tests for rich-text editing (0.9.0 bindings):

set_rich_text replaces the body text *and* the run / paragraph structure
(set_text collapses everything into one run instead), set_justification changes
paragraph alignment, text_fonts lists the document's font set and
set_run_style(font=...) points a run at a font name, appending it to the font
set when it isn't there yet.
"""
import pytest

import psdparse


def _text_layer(p, min_runs=1):
    idx = next((i for i, l in enumerate(p.layers)
                if l.text is not None and len(l.text["runs"]) >= min_runs), None)
    if idx is None:
        pytest.skip("sample has no text layer with enough runs")
    return idx


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def test_set_rich_text_runs_and_paragraphs(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    psd_textbox.set_rich_text(
        i, "AAAA\rBBBB",
        [{"length": 5, "size_px": 30.0, "color": (1.0, 0.0, 0.0)},
         {"length": 5, "size_px": 60.0, "bold": True}],
        [{"length": 5, "justification": 1},
         {"length": 5, "justification": 2}],
    )
    t = _reload(psd_textbox, tmp_path).layers[i].text

    assert t["text"] == "AAAA\rBBBB\r"            # trailing \r appended
    assert [r["length"] for r in t["runs"]] == [5, 5]
    assert t["runs"][0]["size_px"] == pytest.approx(30.0)
    assert tuple(round(c, 3) for c in t["runs"][0]["color"][:3]) == (1.0, 0.0, 0.0)
    assert t["runs"][1]["size_px"] == pytest.approx(60.0)
    assert t["runs"][1]["bold"] is True
    assert [p["justification"] for p in t["paragraphs"]] == [1, 2]
    assert t["justification"] == 1                 # first paragraph


def test_set_rich_text_unspecified_style_inherits_template(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    run0_before = dict(psd_textbox.layers[i].text["runs"][0])

    # only the size is given -- font/colour come from the original first run
    psd_textbox.set_rich_text(i, "XXXX", [{"length": 5, "size_px": 21.0}])
    r = _reload(psd_textbox, tmp_path).layers[i].text["runs"][0]

    assert r["size_px"] == pytest.approx(21.0)
    assert r["font"] == run0_before["font"]
    assert r["color"] == run0_before["color"]


def test_set_rich_text_without_runs_collapses(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    psd_textbox.set_rich_text(i, "ONE RUN")
    t = _reload(psd_textbox, tmp_path).layers[i].text
    assert t["text"] == "ONE RUN\r"
    assert len(t["runs"]) == 1


def test_set_rich_text_length_mismatch_absorbed_by_last_run(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    # 3 + 2 = 5 code units declared for an 11-unit body ("0123456789" + '\r')
    psd_textbox.set_rich_text(i, "0123456789", [{"length": 3}, {"length": 2}])
    t = _reload(psd_textbox, tmp_path).layers[i].text
    assert t["text"] == "0123456789\r"
    assert [r["length"] for r in t["runs"]] == [3, 8]
    assert sum(r["length"] for r in t["runs"]) == len(t["text"])


def test_set_rich_text_font_lands_in_font_set(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    psd_textbox.set_rich_text(i, "font swap", [{"length": 10, "font": "Arial"}])
    q = _reload(psd_textbox, tmp_path)
    assert q.layers[i].text["runs"][0]["font"] == "Arial"
    assert "Arial" in q.text_fonts(i)


def test_set_run_style_font(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox, min_runs=2)
    text_before = psd_textbox.layers[i].text["text"]
    run1_font = psd_textbox.layers[i].text["runs"][1]["font"]

    psd_textbox.set_run_style(i, 0, font="Times New Roman")
    q = _reload(psd_textbox, tmp_path)
    t = q.layers[i].text

    assert t["runs"][0]["font"] == "Times New Roman"
    assert t["runs"][1]["font"] == run1_font          # other runs untouched
    assert t["text"] == text_before
    assert "Times New Roman" in q.text_fonts(i)


def test_set_justification_all_paragraphs(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    n = len(psd_textbox.layers[i].text["paragraphs"])
    if n < 2:
        pytest.skip("sample text layer has a single paragraph")

    psd_textbox.set_justification(i, 2)               # center everything
    t = _reload(psd_textbox, tmp_path).layers[i].text
    assert [p["justification"] for p in t["paragraphs"]] == [2] * n


def test_set_justification_single_paragraph(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    paras = psd_textbox.layers[i].text["paragraphs"]
    if len(paras) < 2:
        pytest.skip("sample text layer has a single paragraph")

    psd_textbox.set_justification(i, 2)
    psd_textbox.set_justification(i, 0, para_index=1)
    t = _reload(psd_textbox, tmp_path).layers[i].text
    got = [p["justification"] for p in t["paragraphs"]]
    assert got[1] == 0
    assert all(j == 2 for k, j in enumerate(got) if k != 1)


def test_set_justification_keeps_text_and_runs(psd_textbox, tmp_path):
    i = _text_layer(psd_textbox)
    before = psd_textbox.layers[i].text
    text_before = before["text"]
    lengths_before = [r["length"] for r in before["runs"]]

    psd_textbox.set_justification(i, 1)
    t = _reload(psd_textbox, tmp_path).layers[i].text
    assert t["text"] == text_before
    assert [r["length"] for r in t["runs"]] == lengths_before


def test_text_fonts_lists_font_set(psd_textbox):
    i = _text_layer(psd_textbox)
    fonts = psd_textbox.text_fonts(i)
    assert isinstance(fonts, list) and fonts
    # every run's resolved font must come from the font set
    for r in psd_textbox.layers[i].text["runs"]:
        assert r["font"] in fonts


def test_text_fonts_non_text_raises(psd_textbox):
    idx = next((i for i, l in enumerate(psd_textbox.layers) if l.text is None), None)
    if idx is None:
        pytest.skip("all layers are text")
    with pytest.raises(RuntimeError):
        psd_textbox.text_fonts(idx)


def test_text_fonts_out_of_range_raises(psd_textbox):
    with pytest.raises(IndexError):
        psd_textbox.text_fonts(999)


def test_set_rich_text_bad_runs_raise(psd_textbox):
    i = _text_layer(psd_textbox)
    with pytest.raises(ValueError):
        psd_textbox.set_rich_text(i, "x", [{"size_px": 10.0}])      # no length
    with pytest.raises(ValueError):
        psd_textbox.set_rich_text(i, "x", ["not a dict"])
    with pytest.raises(ValueError):
        psd_textbox.set_rich_text(i, "x", None, [{"justification": 1}])


def test_set_rich_text_non_text_raises(psd_textbox):
    idx = next((i for i, l in enumerate(psd_textbox.layers) if l.text is None), None)
    if idx is None:
        pytest.skip("all layers are text")
    with pytest.raises(RuntimeError):
        psd_textbox.set_rich_text(idx, "x")


def test_rich_text_valid_for_psd_tools(psd_textbox, tmp_path):
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    i = _text_layer(psd_textbox)
    psd_textbox.set_rich_text(
        i, "赤\r青🌲",
        [{"length": 2, "color": (1.0, 0.0, 0.0), "size_px": 40.0},
         {"length": 4, "color": (0.0, 0.0, 1.0), "italic": True}],
        [{"length": 2, "justification": 0}, {"length": 4, "justification": 2}],
    )
    psd_textbox.set_justification(i, 2, para_index=0)
    dst = tmp_path / "rich.psd"
    assert psd_textbox.save(str(dst))
    PSDImage.open(str(dst)).composite()      # full decode must not raise
