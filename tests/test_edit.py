"""Tests for structural editing + save (E1/E2, 0.7.0):

- record-level parameter setters (opacity / visible / blend mode),
- delete / move / duplicate layers,
- cross-file copy_layer_from,
- the "unmodified files still round-trip byte-identically" invariant,
- the unbalanced-group parse guard (deleting a folder divider must not crash).

Edits only rearrange in-memory references; the actual bytes are re-serialized
on save(). Round-trip identity is preserved for unmodified files because the
original concatenated channel blob is reused until a structural edit dirties it.
"""
import hashlib

import psdparse


def _hash(b):
    return hashlib.md5(b).hexdigest()


def _layer_pixels(p):
    """Per-layer image-pixel hashes, in order (None for empty layers)."""
    out = []
    for i, l in enumerate(p.layers):
        if l.width > 0 and l.height > 0:
            out.append(_hash(p.layer_image(i, "image")))
        else:
            out.append(None)
    return out


def _reload(p, tmp_path, name="out.psd"):
    dst = tmp_path / name
    assert p.save(str(dst))
    q = psdparse.PSDFile()
    assert q.load(str(dst))
    return q


# --- round-trip identity guard --------------------------------------------

def test_unmodified_roundtrip_identical(sample_maskparams_psd, tmp_path):
    p = psdparse.PSDFile()
    assert p.load(str(sample_maskparams_psd))
    dst = tmp_path / "rt.psd"
    assert p.save(str(dst))
    assert _hash(open(sample_maskparams_psd, "rb").read()) == _hash(open(dst, "rb").read())


# --- E1: parameter setters -------------------------------------------------

def test_param_setters_persist(psd_group, tmp_path):
    psd_group.layers[3].opacity = 111
    psd_group.layers[3].visible = False
    psd_group.layers[3].set_blend_mode("mul ")
    q = _reload(psd_group, tmp_path)
    assert q.layers[3].opacity == 111
    assert q.layers[3].visible is False
    assert q.layers[3].blend_mode == psdparse.BlendMode.MULTIPLY


# --- E2: delete ------------------------------------------------------------

def test_delete_layer(psd_group, tmp_path):
    n0 = len(psd_group.layers)
    before = _layer_pixels(psd_group)
    target = 10
    psd_group.delete_layer(target)
    q = _reload(psd_group, tmp_path)
    assert len(q.layers) == n0 - 1
    expected = before[:target] + before[target + 1:]
    assert _layer_pixels(q) == expected


def test_delete_out_of_range_raises(psd_group):
    import pytest
    with pytest.raises(IndexError):
        psd_group.delete_layer(9999)


# --- E2: duplicate ---------------------------------------------------------

def test_duplicate_layer(psd_group, tmp_path):
    n0 = len(psd_group.layers)
    src_hash = _hash(psd_group.layer_image(5, "image"))
    new_index = psd_group.duplicate_layer(5)
    assert new_index == 6
    q = _reload(psd_group, tmp_path)
    assert len(q.layers) == n0 + 1
    assert _hash(q.layer_image(6, "image")) == src_hash


# --- E2: move --------------------------------------------------------------

def test_move_layer(psd_group, tmp_path):
    before = _layer_pixels(psd_group)
    psd_group.move_layer(2, 20)
    q = _reload(psd_group, tmp_path)
    expected = before[:2] + before[3:]
    expected.insert(20, before[2])
    assert _layer_pixels(q) == expected


# --- E2: cross-file copy ---------------------------------------------------

def test_copy_layer_from(psd_group, psd_maskparams, tmp_path):
    n0 = len(psd_group.layers)
    src_hash = _hash(psd_maskparams.layer_image(0, "image"))
    src_name = psd_maskparams.layers[0].name_unicode
    new_index = psd_group.copy_layer_from(psd_maskparams, 0)
    assert new_index == n0
    q = _reload(psd_group, tmp_path)
    assert len(q.layers) == n0 + 1
    assert q.layers[new_index].name_unicode == src_name
    assert _hash(q.layer_image(new_index, "image")) == src_hash


def test_copy_survives_source_gc(psd_group, sample_maskparams_psd, tmp_path):
    # The copied layer references the source's bytes lazily; keep_alive must
    # keep the source object alive until this file is saved even if the local
    # reference is dropped.
    src = psdparse.PSDFile()
    assert src.load(str(sample_maskparams_psd))
    src_hash = _hash(src.layer_image(0, "image"))
    psd_group.copy_layer_from(src, 0)
    del src  # drop our reference; keep_alive on psd_group must hold it
    q = _reload(psd_group, tmp_path)
    assert _hash(q.layer_image(len(q.layers) - 1, "image")) == src_hash


# --- unbalanced-group parse guard -----------------------------------------

def test_delete_folder_divider_no_crash(psd_group, tmp_path):
    # Deleting one half of a FOLDER/HIDDEN divider pair unbalances the group.
    # The result must still load (psdparse used to underflow its parent stack).
    divider = next(
        (i for i, l in enumerate(psd_group.layers)
         if l.layer_type in (psdparse.LayerType.FOLDER, psdparse.LayerType.HIDDEN)),
        None,
    )
    if divider is None:
        import pytest
        pytest.skip("sample has no folder dividers")
    psd_group.delete_layer(divider)
    q = _reload(psd_group, tmp_path)          # must not crash
    assert q.is_loaded
