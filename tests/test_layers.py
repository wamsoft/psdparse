"""Layer enumeration and attribute tests."""
import psdparse


def test_ui_layer_count(psd_ui):
    assert len(psd_ui.layers) == 50


def test_large_layer_count(psd_large):
    assert len(psd_large.layers) == 28


def test_ui_layer_attributes(psd_ui):
    # First non-empty NORMAL layer should have sensible geometry.
    normal = [l for l in psd_ui.layers
              if l.layer_type == psdparse.LayerType.NORMAL
              and l.width > 0 and l.height > 0]
    assert len(normal) > 0
    l = normal[0]
    assert l.width  == l.right - l.left
    assert l.height == l.bottom - l.top
    assert 0 <= l.opacity <= 255


def test_blend_mode_decoding(psd_ui):
    """All decoded layers should have a known (non-INVALID) blend mode."""
    for i, l in enumerate(psd_ui.layers):
        assert l.blend_mode != psdparse.BlendMode.INVALID, \
            f"layer {i} has INVALID blend mode (key=0x{l.blend_mode_key:08x})"


def test_pass_through_in_large(psd_large):
    """The large sample contains group layers with pass-through blending."""
    blend_modes = {l.blend_mode.name for l in psd_large.layers}
    assert "PASS_THROUGH" in blend_modes


def test_layer_indexing(psd_ui):
    l_iter = list(psd_ui.layers)
    l_idx = psd_ui.layers[0]
    assert l_iter[0].left == l_idx.left
    assert l_iter[0].name == l_idx.name


def test_layer_unicode_name(psd_large):
    """At least some layers should have a luni-record unicode name."""
    named = [l for l in psd_large.layers if l.name_unicode]
    assert len(named) > 0


def test_lighter_color_key_is_lgcl(tmp_path):
    """Photoshop writes 'lgCl' for Lighter Color, not 'ltCl'.

    This library long had only 'ltCl', so real-world files decoded as
    BLEND_MODE_INVALID and round-trips silently fell back to Normal.
    'ltCl' is still accepted on read for files this library wrote.
    """
    p = psdparse.PSDFile()
    p.create_blank(4, 4)
    p.add_layer("lighter", 0, 0, bytes([0, 0, 0, 255]) * 16, 4, 4)
    p.layers[0].set_blend_mode("lgCl")
    out = str(tmp_path / "lgcl.psd")
    assert p.save(out)

    q = psdparse.PSDFile()
    assert q.load(out)
    assert q.layers[0].blend_mode == psdparse.BlendMode.LIGHTER_COLOR
    assert q.layers[0].blend_mode_key == 0x6C67436C          # 'lgCl'
