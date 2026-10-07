"""テキストの残りの書式: 行送り / ベースラインシフト / 取り消し線 / 大文字化 /
上付き・下付き / 比率 / 合字 (ラン)、インデント / 段落前後のアキ / 自動行送り /
ハイフネーション (段落)、ワープ。

読み取りは psd-tools のテスト素材 176 ラン分の EngineData と照合済み (開発時)。
ここでは textmask.psd に書き込み → 保存 → 読み直しで往復を確かめ、書いた値が
EngineData に入っていることを psd-tools でも見る。
"""
import pytest

import psdparse

psd_tools = pytest.importorskip("psd_tools")


@pytest.fixture()
def psd(sample_textmask_psd):
    p = psdparse.PSDFile()
    assert p.load(str(sample_textmask_psd))
    return p


def _text_index(p):
    return next(i for i, l in enumerate(p.layers) if l.text)


def _reload(p, tmp_path):
    dst = tmp_path / "out.psd"
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q, dst


def _engine_run(dst, layer_id, run=0):
    t = psd_tools.PSDImage.open(dst)
    l = next(x for x in t.descendants() if x.layer_id == layer_id)
    return l.engine_dict


def test_read_defaults(psd):
    i = _text_index(psd)
    run = psd.layers[i].text["runs"][0]
    for key in ("leading", "baseline_shift", "strikethrough", "font_caps", "font_baseline",
                "horizontal_scale", "vertical_scale", "ligatures"):
        assert key in run
    para = psd.layers[i].text["paragraphs"][0]
    for key in ("first_line_indent", "start_indent", "end_indent", "space_before",
                "space_after", "auto_leading", "hyphenate"):
        assert key in para
    assert psd.layers[i].text["warp"] is None or "style" in psd.layers[i].text["warp"]


def test_run_style_extras_roundtrip(psd, tmp_path):
    i = _text_index(psd)
    psd.set_run_style(i, 0, leading=40.5, baseline_shift=-3.0, strikethrough=True,
                      font_caps=2, font_baseline=1, horizontal_scale=1.25,
                      vertical_scale=0.75, ligatures=False)
    run = psd.layers[i].text["runs"][0]           # 保存前でも新しい値が見える
    assert run["leading"] == pytest.approx(40.5)
    q, dst = _reload(psd, tmp_path)
    run = q.layers[i].text["runs"][0]
    assert run["leading"] == pytest.approx(40.5)
    assert run["baseline_shift"] == pytest.approx(-3.0)
    assert run["strikethrough"] is True
    assert (run["font_caps"], run["font_baseline"]) == (2, 1)
    assert run["horizontal_scale"] == pytest.approx(1.25)
    assert run["vertical_scale"] == pytest.approx(0.75)
    assert run["ligatures"] is False
    ssd = _engine_run(dst, q.layers[i].layer_id)["StyleRun"]["RunArray"][0]["StyleSheet"]["StyleSheetData"]
    assert bool(ssd["AutoLeading"]) is False
    assert float(ssd["Leading"]) == pytest.approx(40.5)
    assert int(ssd["FontCaps"]) == 2


def test_leading_back_to_auto(psd, tmp_path):
    i = _text_index(psd)
    psd.set_run_style(i, 0, leading=30)
    psd.set_run_style(i, 0, leading="auto")
    q, _ = _reload(psd, tmp_path)
    assert q.layers[i].text["runs"][0]["leading"] is None


def test_paragraph_style_roundtrip(psd, tmp_path):
    i = _text_index(psd)
    psd.set_paragraph_style(i, 0, first_line_indent=12, start_indent=4, end_indent=6,
                            space_before=8, space_after=10, auto_leading=1.5,
                            hyphenate=True, justification=2)
    q, dst = _reload(psd, tmp_path)
    para = q.layers[i].text["paragraphs"][0]
    assert (para["first_line_indent"], para["start_indent"], para["end_indent"]) == (12, 4, 6)
    assert (para["space_before"], para["space_after"]) == (8, 10)
    assert para["auto_leading"] == pytest.approx(1.5)
    assert para["hyphenate"] is True
    assert para["justification"] == 2
    props = _engine_run(dst, q.layers[i].layer_id)["ParagraphRun"]["RunArray"][0]["ParagraphSheet"]["Properties"]
    assert float(props["FirstLineIndent"]) == 12
    assert bool(props["AutoHyphenate"]) is True


def test_bad_arguments(psd):
    i = _text_index(psd)
    with pytest.raises(ValueError):
        psd.set_run_style(i, 0, leading="tall")
    with pytest.raises(ValueError):
        psd.set_run_style(i, 0, no_such_style=1)
    with pytest.raises(ValueError):
        psd.set_paragraph_style(i, 0)
    with pytest.raises(ValueError):
        psd.set_paragraph_style(i, 0, indent=3)


def test_rich_text_accepts_new_keys(psd, tmp_path):
    i = _text_index(psd)
    psd.set_rich_text(i, "ab\rcd", runs=[{"length": 3, "strikethrough": True},
                                         {"length": 3, "leading": 50}],
                      paragraphs=[{"length": 3, "space_after": 7}, {"length": 3}])
    q, _ = _reload(psd, tmp_path)
    runs = q.layers[i].text["runs"]
    assert runs[0]["strikethrough"] is True
    assert runs[1]["leading"] == pytest.approx(50)
    assert q.layers[i].text["paragraphs"][0]["space_after"] == 7


def test_warp_read(sample_textmask_psd, tmp_path):
    """TySh の warp descriptor。手元に実例が無いので、psd-tools で textmask.psd の
    ワープを「円弧 50%、水平ゆがみ -20%、垂直方向」に書き換えて読む。"""
    from psd_tools.constants import Tag
    from psd_tools.psd.descriptor import Double, Enumerated
    t = psd_tools.PSDImage.open(sample_textmask_psd)
    layer = next(x for x in t.descendants() if x.kind == "type")
    warp = layer._record.tagged_blocks.get_data(Tag.TYPE_TOOL_OBJECT_SETTING).warp
    warp[b"warpStyle"] = Enumerated(typeID=b"warpStyle", enum=b"warpArc")
    warp[b"warpValue"] = Double(50.0)
    warp[b"warpPerspective"] = Double(-20.0)
    warp[b"warpRotate"] = Enumerated(typeID=b"Ornt", enum=b"Vrtc")
    src = tmp_path / "warp.psd"
    with open(src, "wb") as f:
        t._record.write(f)

    p = psdparse.PSDFile()
    assert p.load(str(src))
    l = next(x for x in p.layers if x.layer_id == layer.layer_id)
    w = l.text["warp"]
    assert w["style"] == "warpArc"
    assert w["value"] == 50.0
    assert w["horizontal_distortion"] == -20.0
    assert w["vertical_distortion"] == 0.0
    assert w["rotate"] == "vertical"
