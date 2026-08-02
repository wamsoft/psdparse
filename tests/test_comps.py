"""Tests for layer comps: document comps (PSDFile.layer_comps) + per-layer
comp state (LayerInfo.comp_states).

Uses Adobe's 'Layer Comps.psd' sample saved as tests/data/layercomps.psd (not
committed — the fixture skips if absent).
"""
import pytest


def test_document_comps(psd_layercomps):
    comps = psd_layercomps.layer_comps
    assert len(comps) >= 1
    for c in comps:
        assert set(c) >= {"id", "name", "record_visibility"}
        # names are null-stripped
        assert "\x00" not in c["name"]


def test_comp_states_shape(psd_layercomps):
    comp_ids = {c["id"] for c in psd_layercomps.layer_comps}
    n_with = 0
    for layer in psd_layercomps.layers:
        st = layer.comp_states
        assert isinstance(st, dict)
        for cid, s in st.items():
            assert isinstance(cid, int)
            assert set(s) == {"enabled", "offset_x", "offset_y"}
            assert isinstance(s["enabled"], bool)
        if st:
            n_with += 1
            # comp ids reference document comps (0 is the "current state" pseudo-id)
            assert set(st).issubset(comp_ids | {0})
    assert n_with > 0


def test_comp_driven_composite(psd_layercomps):
    pytest.importorskip("PIL")
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "examples"))
    from variations import composite_comp
    import numpy as np  # noqa: E402
    for c in psd_layercomps.layer_comps:
        img = composite_comp(psd_layercomps, c["id"])
        assert np.array(img)[..., 3].max() > 0    # non-blank


def test_comp_states_match_psd_tools(psd_layercomps, sample_layercomps_psd):
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    from psd_tools.constants import Tag

    def pt_states(rec):
        tb = rec.tagged_blocks
        md = tb.get_data(Tag.METADATA_SETTING) if tb else None
        if not md:
            return {}
        for item in md:
            if item.key == b"cmls":
                out = {}
                for s in item.data.get(b"layerSettings") or []:
                    cl, e = s.get(b"compList"), s.get(b"enab")
                    if cl is not None and e is not None:
                        out[int(cl[0])] = bool(e)
                return out
        return {}

    pt = PSDImage.open(str(sample_layercomps_psd))
    by_id = {l.layer_id: l for l in psd_layercomps.layers}
    checked = 0
    for l in pt.descendants():
        st = pt_states(l._record)
        if not st or l.layer_id not in by_id:
            continue
        ours = by_id[l.layer_id].comp_states
        for cid, enab in st.items():
            checked += 1
            assert ours.get(cid, {}).get("enabled") == enab
    assert checked > 0
