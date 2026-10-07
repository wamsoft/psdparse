"""ベクタマスク (vmsk / vsms)、保存パス (2000〜2997)、作業パス (1025)、
保存パスの Unicode 名 ('pths') の読み取り。

サンプルは tests/data/make_vector.py が psd-tools で作った vectorsample.psd
(100x50)。座標は文書ピクセルの (x, y) で返る。ファイル上は 8.24 固定小数の
比率なので、比較は 1e-3 px の誤差を許す。
"""
import pytest

import psdparse
from conftest import DATA

TOL = 1e-3


@pytest.fixture(scope="module")
def psd():
    path = DATA / "vectorsample.psd"
    if not path.is_file():
        pytest.skip("vectorsample.psd not generated (run tests/data/make_vector.py)")
    p = psdparse.PSDFile()
    assert p.load(str(path))
    return p


def _anchors(sub):
    return [k["anchor"] for k in sub["knots"]]


def _close(a, b):
    return all(abs(x - y) < TOL for x, y in zip(a, b))


def _close_list(got, want):
    return len(got) == len(want) and all(_close(g, w) for g, w in zip(got, want))


def test_rect_vector_mask(psd):
    vm = psd.layers[0].vector_mask
    assert vm["key"] == "vmsk"
    assert vm["inverted"] is True
    assert vm["not_linked"] is False and vm["disabled"] is False
    path = vm["path"]
    assert path["initial_fill"] == 0
    assert path["clipboard"] is None
    (sub,) = path["subpaths"]
    assert sub["closed"] is True
    assert sub["operation"] == 1
    assert _close_list(_anchors(sub), [(20, 10), (60, 10), (60, 40), (20, 40)])
    for k in sub["knots"]:
        assert k["linked"] is True
        assert _close(k["preceding"], k["anchor"]) and _close(k["leaving"], k["anchor"])


def test_subpath_operation_and_unlinked_knot(psd):
    vm = psd.layers[1].vector_mask
    assert vm["inverted"] is False
    tri, hole = vm["path"]["subpaths"]
    assert (tri["operation"], tri["index"]) == (1, 0)
    assert (hole["operation"], hole["index"]) == (2, 1)       # 前面の型抜き
    apex = tri["knots"][0]
    assert apex["linked"] is False
    assert _close(apex["anchor"], (50, 5))
    assert _close(apex["preceding"], (45, 5))
    assert _close(apex["leaving"], (55, 5))
    assert _close_list(_anchors(hole), [(40, 30), (60, 30), (60, 40), (40, 40)])


def test_saved_and_work_paths(psd):
    paths = psd.paths
    assert [(d["id"], d["kind"]) for d in paths] == [(2000, "saved"), (2001, "saved"), (1025, "work")]

    p1, p2, work = paths
    assert p1["name"] == b"path1"
    assert p1["unicode_name"] == "path1"
    (sub,) = p1["path"]["subpaths"]
    assert sub["closed"] is False
    assert sub["operation"] == -1
    assert _close_list(_anchors(sub), [(5, 5), (95, 45)])

    # リソース名は Shift-JIS の生バイト。Unicode 名は 'pths' から
    assert p2["name"] == "パス".encode("cp932")
    assert p2["unicode_name"] == "パス２"

    assert work["name"] == b""
    assert work["unicode_name"] is None
    assert work["path"]["initial_fill"] == 1
    assert _close_list(_anchors(work["path"]["subpaths"][0]),
                       [(30, 20), (70, 20), (70, 30), (30, 30)])


def test_no_vector_mask_and_no_paths(sample_mask_psd):
    p = psdparse.PSDFile()
    assert p.load(str(sample_mask_psd))
    assert all(l.vector_mask is None for l in p.layers)
    assert p.paths == []


def test_roundtrip_unchanged(psd, tmp_path):
    dst = tmp_path / "out.psd"
    assert psd.save(str(dst))
    assert dst.read_bytes() == (DATA / "vectorsample.psd").read_bytes()


def test_matches_psd_tools(psd):
    """psd-tools の読み方 (比率の (縦, 横)) と一致する"""
    psd_tools = pytest.importorskip("psd_tools")
    t = psd_tools.PSDImage.open(DATA / "vectorsample.psd")
    for i, layer in enumerate(t):
        ours = psd.layers[i].vector_mask["path"]["subpaths"]
        theirs = [s for s in layer.vector_mask.paths]
        assert len(ours) == len(theirs)
        for so, st in zip(ours, theirs):
            assert so["operation"] == st.operation
            for ko, kt in zip(so["knots"], st):
                y, x = kt.anchor
                assert _close(ko["anchor"], (x * t.width, y * t.height))
