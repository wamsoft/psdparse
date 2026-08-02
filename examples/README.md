# psdparse examples

Recipes that combine **psdparse** (structure + pixel extraction + editing) with
the Python imaging ecosystem (Pillow / numpy). psdparse deliberately does *not*
composite or re-render — it hands you the pieces, and these scripts show how to
put them together.

Install the deps: `pip install psdparse pillow numpy`.

## composite.py — composite layers with Pillow

Composites layers bottom-to-top (position + per-layer opacity + mask), **normal
blend only**. Layer effects and non-normal blend modes are not re-rendered
(read `layer.effects` / `layer.blend_mode` and implement them here if needed).

```bash
python composite.py character.psd out.png
python composite.py character.psd out.png --write-back preview.psd
```

`--write-back` writes the composite into a copy of the PSD as its stored preview
via `PSDFile.set_merged_image(...)` — useful after editing layers, since psdparse
does not regenerate the composite itself.

```python
from composite import composite
img = composite(psd)               # PIL.Image of the visible layers
img = composite(psd, show={0, 3})  # only these layer indices
```

On the sample tachie PSDs (all normal blend) this matches Photoshop's stored
composite to ~0.1 mean level difference.

## variations.py — tachie / expression combinations

Game and VTuber character PSDs group mutually-exclusive parts (eyes, mouth,
expression, outfit) into **folders**; each folder is a slot where one option
shows at a time. This enumerates option combinations and composites each.

```bash
python variations.py character.psd out_dir/               # sweep the first group
python variations.py character.psd out_dir/ --group 表情   # sweep a named group
python variations.py doc.psd out_dir/ --comps              # render each Photoshop layer comp
```

`--comps` renders each of the document's Photoshop **layer comps** using
`layer.comp_states` (visibility only; position/appearance overrides aren't
applied). Otherwise it sweeps folder-based option slots.

```python
from variations import option_groups, composite_choices
groups = option_groups(psd)                       # {folder_index: [option_index, ...]}
img = composite_choices(psd, {folder_i: option_i})
```

It treats each top-level folder as a simple "pick one direct child" slot. Real
data sometimes nests (a body folder with an always-on base plus arm options) —
adapt `option_groups` for that.

## extract_layers.py — sprite extraction with alpha bleed

Turns a PSD into per-layer PNGs plus a `manifest.json`, and **extends each
layer's base colour into its transparent area** (alpha bleed / edge dilation) so
the alpha edge doesn't leak a halo when the sprite is rotated or scaled with
bilinear filtering — a standard step when prepping tachie sprites for a game
engine.

```bash
python extract_layers.py character.psd out_dir/ --bleed 8
python extract_layers.py character.psd out_dir/ --no-bleed
```

The bleed only fills RGB under `alpha == 0`; the alpha channel is left untouched.

```python
from extract_layers import alpha_bleed
safe = alpha_bleed(layer_rgba_image, passes=8)
```

## See also

`tools/psd_export.py` (repo root) dumps `layers.json` + `merged.png` + per-layer
PNGs in one shot. The examples here are smaller, focused building blocks you can
copy into your own pipeline.
