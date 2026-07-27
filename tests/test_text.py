"""Text-layer extraction ('TySh' / EngineData).

Validated against tests/data/fontsample.psd, a purpose-built PSD with four
text layers exercising line breaks, emoji (surrogate pairs), vertical text,
and mid-string font/size/color changes.
"""
import psdparse


def _text_layers(p):
    return [l for l in p.layers if l.text is not None]


def test_non_text_layer_returns_none(psd_text):
    # The '背景' raster layer must not be reported as a text layer.
    bg = psd_text.layers[0]
    assert bg.text is None
    assert bg.layer_type != psdparse.LayerType.TEXT


def test_text_layers_detected(psd_text):
    txt = _text_layers(psd_text)
    # fontsample.psd has 4 text layers.
    assert len(txt) == 4
    for l in txt:
        assert l.layer_type == psdparse.LayerType.TEXT


def test_text_dict_shape(psd_text):
    t = _text_layers(psd_text)[0].text
    assert set(t) >= {"text", "orientation", "justification", "transform", "runs"}
    assert isinstance(t["text"], str)
    assert len(t["transform"]) == 6
    assert t["orientation"] in ("horizontal", "vertical")


def test_multiline_content(psd_text):
    # First text layer: three lines separated by CR.
    layer = _text_layers(psd_text)[0]
    assert layer.text["text"] == "普通のテキスト\r二行目\r三行目\r\r"


def test_vertical_orientation(psd_text):
    # The '縦書き' layer is authored as vertical (tate-gaki) text.
    layer = next(l for l in _text_layers(psd_text)
                 if "縦書き" in l.text["text"])
    assert layer.text["orientation"] == "vertical"
    # ...and the horizontal ones report horizontal.
    horiz = [l for l in _text_layers(psd_text)
             if l.text["orientation"] == "horizontal"]
    assert len(horiz) == 3


def test_emoji_surrogate_pairs(psd_text):
    layer = next(l for l in _text_layers(psd_text)
                 if l.text["text"].startswith("🍥"))
    assert layer.text["text"] == "🍥🍑🍒\r"


def test_run_lengths_sum_to_text_length(psd_text):
    # Run lengths are counted in UTF-16 code units (astral chars such as emoji
    # count as 2), matching Photoshop's EngineData RunLengthArray.
    for l in _text_layers(psd_text):
        t = l.text
        assert t["runs"], "expected at least one style run"
        total = sum(r["length"] for r in t["runs"])
        utf16_units = len(t["text"].encode("utf-16-le")) // 2
        assert total == utf16_units


def test_font_names_resolved(psd_text):
    fonts = set()
    for l in _text_layers(psd_text):
        for r in l.text["runs"]:
            fonts.add(r["font"])
    # FontSet index -> name resolution should surface the real families used.
    assert "NotoSansJP-Thin" in fonts
    assert "SourceHanSansJP-Normal" in fonts


def test_mid_string_style_changes(psd_text):
    # The layer whose name advertises font/size/color changes mid-string.
    layer = next(l for l in _text_layers(psd_text)
                 if l.text["text"].startswith("フォントを途中でかえる"))
    runs = layer.text["runs"]
    # more than one distinct font, size, and color across runs
    assert len({r["font"] for r in runs}) >= 2
    assert len({r["size_px"] for r in runs}) >= 2
    # a blue run (B channel dominant) exists among otherwise-red text
    assert any(r["color"] and r["color"][2] > 0.5 for r in runs)
    assert any(r["color"] and r["color"][0] > 0.5 for r in runs)


def test_run_has_tracking_kerning(psd_text):
    for l in _text_layers(psd_text):
        for r in l.text["runs"]:
            assert isinstance(r["tracking"], int)
            assert isinstance(r["kerning"], int)
            assert isinstance(r["auto_kerning"], bool)
    # fontsample.psd was authored with -100 tracking across its runs.
    all_runs = [r for l in _text_layers(psd_text) for r in l.text["runs"]]
    assert any(r["tracking"] == -100 for r in all_runs)


def test_color_is_rgba_alpha_last(psd_text):
    for l in _text_layers(psd_text):
        for r in l.text["runs"]:
            if r["color"] is not None:
                assert len(r["color"]) == 4
                # opaque text -> alpha (last component) == 1.0
                assert r["color"][3] == 1.0
