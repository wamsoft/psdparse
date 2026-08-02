"""Extract each layer to a PNG, with alpha edge-extension (bleed), + a manifest.

A common step when turning a tachie PSD into game-ready sprites: pull each part
out as its own image, and **extend the base colour into the transparent area** so
the alpha edge doesn't leak a dark/transparent halo when the sprite is rotated or
scaled with bilinear filtering. This writes:

    out_dir/000_<name>.png    ... each pixel layer (RGB bled under the alpha)
    out_dir/manifest.json     ... index/name/position/size/opacity/blend/parent

    python extract_layers.py character.psd out_dir/ [--bleed 8] [--no-bleed]

Needs numpy (for the bleed). The bleed leaves the alpha channel untouched — it
only fills RGB in transparent pixels, which is exactly what edge-safe scaling
wants.
"""
from __future__ import annotations

import argparse
import json
import os

import numpy as np
import psdparse
from PIL import Image

from composite import _PIXEL_TYPES, layer_rgba


def alpha_bleed(img: Image.Image, passes: int = 8) -> Image.Image:
    """Extend RGB into transparent pixels by iterative dilation (`passes` px).
    Alpha is preserved; only the colour under alpha==0 is filled from neighbours."""
    arr = np.array(img.convert("RGBA"))
    rgb = arr[..., :3].astype(np.float32)
    filled = arr[..., 3] > 0
    for _ in range(passes):
        if filled.all():
            break
        color_sum = np.zeros_like(rgb)
        count = np.zeros(filled.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            m = np.roll(filled, (dy, dx), (0, 1))
            shifted = np.roll(rgb, (dy, dx), (0, 1))
            if dy == 1:    m[0, :] = False       # kill np.roll wrap-around
            elif dy == -1: m[-1, :] = False
            if dx == 1:    m[:, 0] = False
            elif dx == -1: m[:, -1] = False
            color_sum += shifted * m[..., None]
            count += m
        newly = (~filled) & (count > 0)
        rgb[newly] = color_sum[newly] / count[newly][..., None]
        filled |= newly
    arr[..., :3] = np.clip(rgb, 0, 255).astype(np.uint8)
    return Image.fromarray(arr, "RGBA")


def _blend_key(layer: psdparse.LayerInfo) -> str:
    k = layer.blend_mode_key
    return "".join(chr((k >> s) & 0xFF) for s in (24, 16, 8, 0))


def main() -> None:
    ap = argparse.ArgumentParser(description="Extract layers to PNGs with alpha bleed.")
    ap.add_argument("input")
    ap.add_argument("out_dir")
    ap.add_argument("--bleed", type=int, default=8, help="edge-extension passes (px)")
    ap.add_argument("--no-bleed", action="store_true")
    args = ap.parse_args()

    psd = psdparse.PSDFile()
    if not psd.load(args.input):
        raise SystemExit(f"failed to load {args.input}")
    os.makedirs(args.out_dir, exist_ok=True)

    manifest = {"width": psd.header.width, "height": psd.header.height, "layers": []}
    for i, layer in enumerate(psd.layers):
        if layer.layer_type not in _PIXEL_TYPES:
            continue
        img = layer_rgba(psd, i)
        if img is None:
            continue
        if not args.no_bleed and args.bleed > 0:
            img = alpha_bleed(img, args.bleed)
        try:
            name = layer.name_unicode
        except UnicodeDecodeError:
            name = f"layer{i}"
        safe = "".join(c if c.isalnum() or c in "._-()（）　 " else "_" for c in name).strip()
        fname = f"{i:03d}_{safe or 'layer'}.png"
        img.save(os.path.join(args.out_dir, fname))
        manifest["layers"].append({
            "index": i, "name": name, "file": fname,
            "left": layer.left, "top": layer.top,
            "width": layer.width, "height": layer.height,
            "opacity": layer.opacity, "blend": _blend_key(layer),
            "visible": layer.visible, "parent_index": layer.parent_index,
        })

    with open(os.path.join(args.out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    print(f"extracted {len(manifest['layers'])} layers to {args.out_dir}")


if __name__ == "__main__":
    main()
