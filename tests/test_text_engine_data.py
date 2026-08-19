"""Tests for Txt2 — the document-wide Text Engine Data (0.8.0).

Photoshop CS3 and later keep the whole document's text engine state in a `Txt2`
block at the end of the layer & mask section, and **read it in preference to
each layer's `TySh`**. So rewriting `TySh` alone leaves the edit invisible in
Photoshop: it shows the old text, not just a stale raster.

psdparse therefore either mirrors a text edit into `Txt2` (`TEXTENGINE_SYNC`,
the default) or drops the block so Photoshop falls back to `TySh`
(`TEXTENGINE_REMOVE`). Changes that cannot be mirrored — style, alignment,
box, placement — drop it too rather than leave the old text behind.
"""
import pytest

import psdparse

SYNC, REMOVE, KEEP = 0, 1, 2


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


def _tysh_texts(p):
    return [p.layers[i].text["text"] for i in _text_layers(p)]


def test_txt2_present(psd_textbox):
    assert psd_textbox.has_text_engine_data()
    assert psd_textbox.document_additional_info("Txt2")


def test_txt2_serializer_byte_exact(psd_textbox):
    # Txt2 uses the same mini-language as EngineData but is written as a single
    # space-separated line with numeric key aliases. Round-tripping it must
    # reproduce the exact bytes, or every edit would rewrite unrelated values.
    blob = psd_textbox.document_additional_info("Txt2")
    assert psdparse._reserialize_text_engine_data(blob) == blob


def test_txt2_order_matches_text_index(psd_textbox):
    # The body objects inside Txt2 line up with each layer's TySh TextIndex.
    texts = psd_textbox.text_engine_texts()
    for i in _text_layers(psd_textbox):
        ti = psd_textbox.layer_text_index(i)
        assert ti is not None
        assert texts[ti] == psd_textbox.layers[i].text["text"]


def test_untouched_save_is_byte_identical(psd_textbox, sample_textbox_psd, tmp_path):
    # The Txt2 support carves the trailing section into blocks; an untouched
    # save must still come back byte for byte.
    dst = tmp_path / "noop.psd"
    assert psd_textbox.save(str(dst))
    assert dst.read_bytes() == sample_textbox_psd.read_bytes()


def test_set_text_syncs_txt2(psd_textbox, tmp_path):
    i = _text_layers(psd_textbox)[0]
    ti = psd_textbox.layer_text_index(i)
    psd_textbox.set_text(i, "replaced")

    assert psd_textbox.has_text_engine_data()
    assert not psd_textbox.text_engine_dropped
    assert psd_textbox.text_engine_texts()[ti] == "replaced\r"

    q = _reload(psd_textbox, tmp_path)
    assert q.text_engine_texts()[ti] == "replaced\r"
    assert q.layers[i].text["text"] == "replaced\r"


def test_sync_handles_longer_and_shorter_text(psd_textbox, tmp_path):
    # Growing and shrinking both have to keep Txt2 parseable: the run lengths
    # move with the text, and Photoshop recomputes the layout cache itself.
    idx = _text_layers(psd_textbox)
    long_text = "\r".join(f"line {n}" for n in range(8))
    psd_textbox.set_text(idx[0], long_text)
    psd_textbox.set_text(idx[1], "x")

    q = _reload(psd_textbox, tmp_path)
    assert q.text_engine_texts()[q.layer_text_index(idx[0])] == long_text + "\r"
    assert q.text_engine_texts()[q.layer_text_index(idx[1])] == "x\r"
    blob = q.document_additional_info("Txt2")
    assert psdparse._reserialize_text_engine_data(blob) == blob


def test_rich_text_without_style_syncs(psd_textbox, tmp_path):
    i = _text_layers(psd_textbox)[0]
    ti = psd_textbox.layer_text_index(i)
    text = "aa\rbbb\r"
    psd_textbox.set_rich_text(
        i, text,
        runs=[{"length": 3}, {"length": 4}],
        paragraphs=[{"length": 3}, {"length": 4}],
    )
    assert not psd_textbox.text_engine_dropped
    q = _reload(psd_textbox, tmp_path)
    assert q.text_engine_texts()[ti] == text


def test_rich_text_with_style_drops_txt2(psd_textbox, tmp_path):
    # A style override cannot be mirrored into Txt2's own style sheets, so the
    # block is dropped instead of being left holding the old text.
    i = _text_layers(psd_textbox)[0]
    psd_textbox.set_rich_text(i, "styled", runs=[{"length": 7, "size_px": 40}])
    assert psd_textbox.text_engine_dropped
    assert not psd_textbox.has_text_engine_data()

    q = _reload(psd_textbox, tmp_path)
    assert not q.has_text_engine_data()
    assert q.layers[i].text["text"] == "styled\r"


def test_formatting_unchanged_opt_in_syncs(psd_textbox, tmp_path):
    # Callers that always pass absolute styles (psdtext does, so that "no tag"
    # means "the base style" rather than "whatever was there") cannot signal a
    # text-only edit through the presence of style fields. They say so directly.
    i = _text_layers(psd_textbox)[0]
    ti = psd_textbox.layer_text_index(i)
    psd_textbox.set_rich_text(i, "same look", runs=[{"length": 10, "size_px": 40}],
                              formatting_unchanged=True)
    assert not psd_textbox.text_engine_dropped
    q = _reload(psd_textbox, tmp_path)
    assert q.text_engine_texts()[ti] == "same look" + chr(13)


def test_justification_drops_txt2(psd_textbox):
    i = _text_layers(psd_textbox)[0]
    psd_textbox.set_justification(i, 2)
    assert psd_textbox.text_engine_dropped
    assert not psd_textbox.has_text_engine_data()


def test_text_bounds_drops_txt2(psd_textbox):
    # The box drives wrapping, and Txt2 carries its own copy of it.
    i = _text_layers(psd_textbox)[0]
    psd_textbox.set_text_bounds(i, 0, 0, 100, 100)
    assert not psd_textbox.has_text_engine_data()


def test_policy_remove(psd_textbox, tmp_path):
    i = _text_layers(psd_textbox)[0]
    psd_textbox.set_text_engine_policy(REMOVE)
    assert psd_textbox.text_engine_policy == REMOVE
    assert not psd_textbox.has_text_engine_data()   # dropped right away

    psd_textbox.set_text(i, "removed")
    q = _reload(psd_textbox, tmp_path)
    assert not q.has_text_engine_data()
    assert q.layers[i].text["text"] == "removed\r"


def test_policy_keep(psd_textbox, tmp_path):
    # KEEP is the pre-0.8.0 behaviour: Txt2 stays as it was, so Photoshop keeps
    # showing the old text. Only useful when the caller knows what it wants.
    i = _text_layers(psd_textbox)[0]
    before = psd_textbox.text_engine_texts()[psd_textbox.layer_text_index(i)]
    psd_textbox.set_text_engine_policy(KEEP)
    psd_textbox.set_text(i, "ignored by photoshop")

    q = _reload(psd_textbox, tmp_path)
    assert q.has_text_engine_data()
    assert q.text_engine_texts()[q.layer_text_index(i)] == before
    assert q.layers[i].text["text"] == "ignored by photoshop\r"


def test_drop_is_idempotent(psd_textbox, tmp_path):
    assert psd_textbox.drop_text_engine_data()
    assert psd_textbox.drop_text_engine_data()
    assert not psd_textbox.has_text_engine_data()
    q = _reload(psd_textbox, tmp_path)
    assert not q.has_text_engine_data()
    # Everything else has to survive the block being cut out.
    assert _tysh_texts(q) == _tysh_texts(psd_textbox)


def test_masked_text_layers_sync(psd_textmask, tmp_path):
    # Text layer + mask is the shape that shows up in real work; the mask must
    # survive a Txt2-syncing edit.
    i = _text_layers(psd_textmask)[0]
    ti = psd_textmask.layer_text_index(i)
    had_mask = psd_textmask.layers[i].mask is not None
    psd_textmask.set_text(i, "masked")

    q = _reload(psd_textmask, tmp_path)
    assert q.text_engine_texts()[ti] == "masked\r"
    assert (q.layers[i].mask is not None) == had_mask
