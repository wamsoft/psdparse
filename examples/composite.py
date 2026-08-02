"""Composite psdparse layers with Pillow.

psdparse gives you each layer's pixels (`layer_image`), position, opacity and
visibility; the actual compositing is done here in Python with Pillow. This keeps
the C++ core lean and lets you use the mature imaging ecosystem.

Scope: **normal blend** + per-layer opacity + mask (via ``layer_image(i,
"masked")``) + position, composited bottom-to-top. Non-normal blend modes and
layer effects are *not* applied (psdparse does not re-render effects); if you
need those, read `layer.blend_mode` / `layer.effects` and implement them here.

    python composite.py input.psd output.png [--write-back edited.psd]

As a library:

    from composite import composite
    img = composite(psd)            # PIL.Image, currently-visible layers
    img = composite(psd, show={0, 3, 7})   # only these layer indices
"""
from __future__ import annotations

import argparse

import psdparse
from PIL import Image

# Layer types that carry pixels (skip folder/divider markers).
_PIXEL_TYPES = (
    psdparse.LayerType.NORMAL,
    psdparse.LayerType.TEXT,
    psdparse.LayerType.ADJUST,
    psdparse.LayerType.FILL,
)


def layer_rgba(psd: psdparse.PSDFile, index: int) -> Image.Image | None:
    """A layer's masked pixels as an RGBA PIL image (opacity folded into alpha),
    or None for empty layers."""
    layer = psd.layers[index]
    if layer.width <= 0 or layer.height <= 0:
        return None
    bgra = psd.layer_image(index, "masked")            # mask applied to alpha
    img = Image.frombytes("RGBA", (layer.width, layer.height), bgra, "raw", "BGRA")
    if layer.opacity < 255:
        alpha = img.getchannel("A").point(lambda v: v * layer.opacity // 255)
        img.putalpha(alpha)
    return img


def _paste_clipped(canvas: Image.Image, img: Image.Image, x: int, y: int) -> None:
    """alpha-composite `img` onto `canvas` at (x, y), clipping to the canvas
    (handles layers that extend past the edges / negative offsets)."""
    cw, ch = canvas.size
    w, h = img.size
    sx0, sy0 = max(0, -x), max(0, -y)
    sx1, sy1 = min(w, cw - x), min(h, ch - y)
    if sx1 <= sx0 or sy1 <= sy0:
        return
    crop = img.crop((sx0, sy0, sx1, sy1))
    tmp = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    tmp.paste(crop, (max(0, x), max(0, y)))
    canvas.alpha_composite(tmp)


def composite(psd: psdparse.PSDFile, show: set[int] | None = None) -> Image.Image:
    """Composite layers bottom-to-top into a canvas-sized RGBA image.

    `show`: set of layer indices to include. Default (None) = every currently
    **visible** pixel layer. Folder/divider markers are always skipped.
    """
    canvas = Image.new("RGBA", (psd.header.width, psd.header.height), (0, 0, 0, 0))
    for i, layer in enumerate(psd.layers):          # index 0 = bottom-most
        if layer.layer_type not in _PIXEL_TYPES:
            continue
        if show is None:
            if not layer.visible:
                continue
        elif i not in show:
            continue
        img = layer_rgba(psd, i)
        if img is not None:
            _paste_clipped(canvas, img, layer.left, layer.top)
    return canvas


def main() -> None:
    ap = argparse.ArgumentParser(description="Composite visible layers with Pillow.")
    ap.add_argument("input")
    ap.add_argument("output", help="output PNG")
    ap.add_argument("--write-back", metavar="PSD",
                    help="also write the composite into a copy of the PSD "
                         "(as the stored preview) via set_merged_image")
    args = ap.parse_args()

    psd = psdparse.PSDFile()
    if not psd.load(args.input):
        raise SystemExit(f"failed to load {args.input}")
    img = composite(psd)
    img.save(args.output)
    print(f"wrote {args.output} ({img.width}x{img.height})")

    if args.write_back:
        # set_merged_image wants canvas-sized BGRA
        bgra = img.convert("RGBA").tobytes("raw", "BGRA")
        psd.set_merged_image(bgra)
        psd.save(args.write_back)
        print(f"wrote {args.write_back} with the composite as its stored preview")


if __name__ == "__main__":
    main()
