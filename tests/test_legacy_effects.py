"""旧形式のレイヤー効果 'lrFX' (layer.legacy_effects)。

psd-tools の低レベル API で、masktest.psd のレイヤにドロップシャドウ / 外側光彩 /
ベベル / 塗りの 'lrFX' を付けたファイルをその場で作って読む。
"""
import pytest

import psdparse

psd_tools = pytest.importorskip("psd_tools")


def _make(src, dst):
    from psd_tools.constants import BlendMode, ColorSpaceID, Tag
    from psd_tools.psd.color import Color
    from psd_tools.psd.effects_layer import (
        BevelInfo, CommonStateInfo, EffectsLayer, OuterGlowInfo, ShadowInfo, SolidFillInfo,
    )
    from psd_tools.psd.tagged_blocks import TaggedBlock
    from psd_tools.constants import EffectOSType

    rgb = lambda r, g, b: Color(ColorSpaceID.RGB, [r, g, b, 0])
    fx = EffectsLayer(version=0, items=[
        (EffectOSType.COMMON_STATE, CommonStateInfo(version=0, visible=1)),
        (EffectOSType.DROP_SHADOW, ShadowInfo(
            version=2, blur=5 << 16, intensity=0, angle=120, distance=7 << 16,
            color=rgb(65535, 0, 0), blend_mode=BlendMode.MULTIPLY, enabled=1,
            use_global_angle=0, opacity=191, native_color=rgb(65535, 0, 0))),
        (EffectOSType.OUTER_GLOW, OuterGlowInfo(
            version=2, blur=3 << 16, intensity=0, color=rgb(0, 65535, 0),
            blend_mode=BlendMode.SCREEN, enabled=0, opacity=128, native_color=rgb(0, 65535, 0))),
        (EffectOSType.BEVEL, BevelInfo(
            version=2, angle=30, depth=4, blur=2, highlight_blend_mode=BlendMode.SCREEN,
            shadow_blend_mode=BlendMode.MULTIPLY, highlight_color=rgb(65535, 65535, 65535),
            shadow_color=rgb(0, 0, 0), bevel_style=1, highlight_opacity=191,
            shadow_opacity=191, enabled=1, use_global_angle=1, direction=0,
            real_highlight_color=rgb(65535, 65535, 65535), real_shadow_color=rgb(0, 0, 0))),
        (EffectOSType.SOLID_FILL, SolidFillInfo(
            version=2, blend_mode=BlendMode.NORMAL, color=rgb(0, 0, 65535), opacity=255,
            enabled=1, native_color=rgb(0, 0, 65535))),
    ])
    t = psd_tools.PSDImage.open(src)
    rec = t._record
    r = rec.layer_and_mask_information.layer_info.layer_records[0]
    r.tagged_blocks[Tag.EFFECTS_LAYER] = TaggedBlock(key=Tag.EFFECTS_LAYER, data=fx)
    with open(dst, "wb") as f:
        rec.write(f)


def test_legacy_effects(sample_mask_psd, tmp_path):
    dst = tmp_path / "lrfx.psd"
    try:
        _make(sample_mask_psd, dst)
    except (ImportError, TypeError) as e:          # psd-tools の API が違う版
        pytest.skip(f"psd-tools cannot build lrFX here: {e}")
    p = psdparse.PSDFile()
    assert p.load(str(dst))
    fx = p.layers[0].legacy_effects
    assert set(fx) == {"common_state", "drop_shadow", "outer_glow", "bevel", "solid_fill"}
    assert fx["common_state"]["visible"] == 1
    ds = fx["drop_shadow"]
    assert (ds["blur"], ds["distance"], ds["angle"], ds["opacity"]) == (5 << 16, 7 << 16, 120, 191)
    assert ds["color"] == [0, 65535, 0, 0, 0]          # [色空間 0 = RGB, R, G, B, -]
    assert ds["blend_mode"] == "mul "
    assert fx["outer_glow"]["enabled"] == 0 and fx["outer_glow"]["opacity"] == 128
    bv = fx["bevel"]
    assert (bv["angle"], bv["depth"], bv["bevel_style"], bv["direction"]) == (30, 4, 1, 0)
    assert bv["highlight_blend_mode"] == "scrn"
    assert fx["solid_fill"]["color"] == [0, 0, 0, 65535, 0]
    assert not any(v.get("incomplete") for v in fx.values())


def test_no_lrfx(sample_mask_psd):
    p = psdparse.PSDFile()
    assert p.load(str(sample_mask_psd))
    assert p.layers[0].legacy_effects is None
