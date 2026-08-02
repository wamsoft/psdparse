"""Tests for layer-effect (lfx2) value editing (E3 effects, 0.7.0):

A full Photoshop descriptor serializer (the inverse of the parser) is validated
byte-for-byte, then used to edit effect values. Edits merge onto the parsed
typed descriptor so structure / class IDs / types and all untouched values are
preserved.
"""
import hashlib

import pytest

import psdparse


def _effects_layer(p):
    idx = next((i for i, l in enumerate(p.layers) if l.effects is not None), None)
    if idx is None:
        pytest.skip("sample has no lfx2 effects")
    return idx


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


def test_serializer_byte_exact(psd_group):
    # Re-serializing an unchanged descriptor must reproduce the exact bytes.
    idx = _effects_layer(psd_group)
    before = psd_group.layers[idx].descriptor_bytes("lfx2")
    psd_group.set_effects(idx, {})
    after = psd_group.layers[idx].descriptor_bytes("lfx2")
    assert before == after


def test_reconstruction_whole_file_identical(sample_group_psd, tmp_path):
    # set_effects({}) forces full extra-data reconstruction; the whole saved
    # file must stay byte-identical to the original.
    p = psdparse.PSDFile()
    assert p.load(str(sample_group_psd))
    idx = _effects_layer(p)
    p.set_effects(idx, {})
    dst = tmp_path / "noop.psd"
    assert p.save(str(dst))
    assert hashlib.md5(open(sample_group_psd, "rb").read()).hexdigest() == \
           hashlib.md5(open(dst, "rb").read()).hexdigest()


def test_edit_effect_values(psd_group, tmp_path):
    idx = _effects_layer(psd_group)
    before = psd_group.layers[idx].effects
    other_keys = [k for k in before if k not in ("masterFXSwitch",)]
    snapshot = {k: before[k] for k in other_keys if not isinstance(before[k], dict)}

    psd_group.set_effects(idx, {"masterFXSwitch": False})
    q = _reload(psd_group, tmp_path)
    fx = q.layers[idx].effects
    assert fx["masterFXSwitch"] is False
    # untouched scalar values preserved
    for k, v in snapshot.items():
        assert fx[k] == v


def test_edit_nested_unitfloat(psd_group, tmp_path):
    idx = _effects_layer(psd_group)
    fx = psd_group.layers[idx].effects
    pf = fx.get("patternFill")
    if not isinstance(pf, dict) or "Opct" not in pf:
        pytest.skip("sample effect has no patternFill.Opct")
    psd_group.set_effects(idx, {"patternFill": {"Opct": {"value": 50.0}}})
    q = _reload(psd_group, tmp_path)
    assert q.layers[idx].effects["patternFill"]["Opct"]["value"] == pytest.approx(50.0)


def test_unknown_key_ignored(psd_group, tmp_path):
    idx = _effects_layer(psd_group)
    before = psd_group.layers[idx].effects
    psd_group.set_effects(idx, {"nonexistentKeyXYZ": 123})
    q = _reload(psd_group, tmp_path)
    assert q.layers[idx].effects.keys() == before.keys()


def test_set_effects_no_lfx2_raises(psd_group):
    # find a layer without effects
    idx = next((i for i, l in enumerate(psd_group.layers) if l.effects is None), None)
    if idx is None:
        pytest.skip("all layers have effects")
    with pytest.raises(RuntimeError):
        psd_group.set_effects(idx, {"masterFXSwitch": False})


def test_edited_effects_valid_for_psd_tools(psd_group, tmp_path):
    PSDImage = pytest.importorskip("psd_tools").PSDImage
    idx = _effects_layer(psd_group)
    psd_group.set_effects(idx, {"masterFXSwitch": False})
    dst = tmp_path / "fx.psd"
    assert psd_group.save(str(dst))
    PSDImage.open(str(dst)).composite()   # full decode must not raise
