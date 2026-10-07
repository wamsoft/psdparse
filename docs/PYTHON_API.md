# psdparse Python API Reference

The Python bindings expose a small surface area focused on **reading** PSDs, **extracting raw pixel data**, and **saving** a loaded file back as PSD. Pixel data is returned as raw BGRA `bytes` objects suitable for handing to PIL, NumPy, etc.

All public path arguments are Python `str`. pybind11 transparently encodes them as UTF-8 before reaching the C++ layer.

The bindings cover the C++ library's public editing surface; the authoritative
C++ headers are `psdparse/psdfile.h` + `psdparse/psdengine.h`.

```python
import psdparse
```

## `class psdparse.PSDFile`

### Constructor

```python
p = psdparse.PSDFile()
```

### Loading

```python
p.load(path: str) -> bool
```

Memory-map the file at `path` and parse. On Windows the path is converted UTF-8 → UTF-16 internally before `CreateFileMappingW`. Layer pixels are paged in lazily — the parse step only reads structural metadata (a few hundred KB even for very large PSDs).

```python
p.load_bytes(data: bytes) -> bool
```

Parse a PSD already loaded into a Python `bytes`. The bytes are copied into an internal `std::vector`. Useful when the file came from somewhere other than disk.

```python
p.load_streamed(path: str) -> bool
```

Open `path` as a `std::ifstream` and parse via `StreamReader`. Functionally equivalent to `load()` but exercises the stream code path — handy for testing and for environments where mmap isn't appropriate (network paths, etc.).

### Saving

```python
p.save(path: str) -> bool
```

Save the currently loaded (and optionally edited) data back to disk as PSD.
An **unmodified** file round-trips byte-identically (`p.load(a); p.save(b)`),
and edits are re-serialized on save — see [Editing & saving](#editing--saving).
Do not save over a file that is currently loaded (returns `False`; see
[Saving](#saving)).

### Header

```python
p.header.width       # int
p.header.height      # int
p.header.channels    # int
p.header.depth       # int (8 / 16 / 32)
p.header.mode        # int (use psdparse.COLOR_MODE_* constants to compare)
p.header.version     # int (1 = PSD, 2 = PSB)
p.header.is_psb      # bool -- version == 2 (large document format)
```

### Layers

```python
p.layers             # list[LayerInfo] -- read-only, flat, bottom-to-top
p.roots              # list[int] -- top-level layer indices (tree view)
p.children(i)        # list[int] -- direct children of layers[i]; i=-1 for roots
p.merged_alpha       # bool
p.is_loaded          # bool
p.layer_source       # None, or "Lr16" / "Lr32" when a 16/32-bit document keeps
                     # its layers in that document-level block (save() rewrites it)
```

See [Layer hierarchy](#layer-hierarchy) for how the flat list and the tree view
relate.

### Image extraction

```python
p.merged_image() -> bytes
```

Returns the composite image as raw BGRA bytes. Length = `width * height * 4`. Raises `RuntimeError` if the PSD didn't store a composite (rare).

```python
p.layer_image(index: int, mode: str = "masked") -> bytes
```

Returns the pixels of one layer as raw BGRA bytes.

- `mode="masked"` (default) — the image with the layer mask applied to alpha
- `mode="image"` — the image only, ignoring mask
- `mode="mask"` — the mask only, rendered as grayscale-in-BGRA

Length = `layer.width * layer.height * 4`. Returns `b""` for empty layers (`width == 0` or `height == 0`). Raises `IndexError` on bad index, `ValueError` on bad mode.

### Compositing from layers (0.13.0)

```python
p.composite(effects: bool = True, background: tuple | None = None) -> (bytes, dict)
```

Renders the document from its layers instead of returning the stored merged
image: blend modes (including Photoshop's special handling of fill opacity for
linear burn / color dodge / hard mix and the like), pass-through / isolated
groups, clipping (layer, group or adjustment layer as the base), knockout,
layer and vector masks (density / feather), fill layers (solid / gradient /
pattern), shapes with their strokes, adjustment layers (all but color lookup),
fill opacity, channel restriction, artboards, and layer effects when
`effects=True`. Returns `(bgra, stats)` — canvas-sized BGRA (straight alpha)
and a dict counting what could not be reproduced:
`skipped_adjustments` (e.g. color lookup), `unsupported_clip_base`,
`unsupported_effects` (e.g. noise gradients). See
[SUPPORT.md](SUPPORT.md#再合成-composite-0130) for how closely each
part follows Photoshop.
`background=(r, g, b)` composites onto an opaque colour instead of
transparency.

Large images are split across threads (rows of each blend / adjustment /
blur / distance pass); the result is identical to a single-threaded run.

```python
psdparse.set_threads(count: int) -> None   # 0 = automatic (default), 1 = single-threaded
psdparse.get_threads() -> int
```

The automatic count is the number of logical cores, capped at 16; the
`PSDFX_THREADS` environment variable overrides it when `set_threads` was not
called.

```python
p.render_layer(index: int, effects: bool = True) -> (bytes, left, top, width, height) | None
```

Renders a single layer (masks, fill / shape content, opacity and effects) onto
a transparent surface, not composited with the layers below. The rectangle
includes what the effects add around the layer (shadow, glow, stroke). `None`
for groups, dividers, adjustment layers and empty layers.

```python
p.merged_has_transparency -> bool
```

Whether the stored merged image carries transparency. `merged_image()` is
un-matted from the white background Photoshop composites transparent documents
onto, so it can be compared with `composite()` directly.

```python
out, stats = p.composite()
p.set_merged_image(out)          # refresh the stored composite after edits
```

The drawing code is a plain C API (`psdparse/psdfx.h`, see
[PSDFX.md](PSDFX.md)), so other hosts can call the same routines.

## `class psdparse.LayerInfo`

Read-only view of one layer.

| Attribute | Type | Notes |
|---|---|---|
| `top, left, bottom, right` | `int` | layer bounding box on canvas |
| `width, height` | `int` | derived from bbox |
| `opacity` | `int` | 0..255 |
| `fill_opacity` | `int` | 0..255. **Writable** (the `iOpa` block) |
| `clipping` | `int` | 0=base, 1=non-base |
| `blend_mode_key` | `int` | raw 4cc value (e.g. `'norm'` as int) |
| `blend_mode` | `BlendMode` enum | parsed blend mode |
| `layer_type` | `LayerType` enum | NORMAL / HIDDEN / FOLDER / ADJUST / FILL / TEXT |
| `layer_id` | `int` | -1 if unset |
| `channels` | `list[ChannelInfo]` | per-channel id+length |
| `name` | `str` | raw Pascal-string name (CP932 etc on Japanese PSDs — pybind11 may raise UnicodeDecodeError when read) |
| `name_unicode` | `str` | UTF-16 Unicode name from `luni` record (preferred). **Writable** — assigning renames the layer (see [Editing](#editing--saving)) |
| `parent_index` | `int` | index into `PSDFile.layers` of the enclosing folder, or `-1` for top level — see [Layer hierarchy](#layer-hierarchy) |
| `children` | `list[int]` | indices of the direct children, bottom-to-top; empty for non-folders. Dividers excluded — see [Layer hierarchy](#layer-hierarchy) |
| `is_group` | `bool` | `True` for `FOLDER` layers |
| `text` | `dict` \| `None` | text-layer content & style (`None` for non-text layers) — see below |
| `mask` | `dict` \| `None` | layer mask geometry & flags (`None` when the layer has no mask) — see below |
| `blending_ranges` | `dict` \| `None` | "Blend If" ranges (`None` when absent) — see below |
| `effects` | `dict` \| `None` | layer effects (`lfx2`) as a descriptor dict — see [Descriptor blocks](#descriptor-blocks) |
| `fill` | `dict` \| `None` | fill-layer content (solid/gradient/pattern) — see [Descriptor blocks](#descriptor-blocks) |
| `name_raw` | `bytes` | the Pascal layer name as stored (system encoding). `name` decodes it as UTF-8 and falls back to `name_unicode` (or replacement characters) instead of raising |
| `sheet_color` | `dict` \| `None` | layer-panel color label (`lclr`): `{"index", "name"}` — `None` when no `lclr` block |
| `legacy_effects` | `dict` \| `None` | old-style effects (`lrFX`) as `{type: values}`: `common_state`, `drop_shadow`, `inner_shadow`, `outer_glow`, `inner_glow`, `bevel`, `solid_fill`; colors are `[color_space, c0, c1, c2, c3]`, blend modes 4-char keys, blur / intensity / distance the stored 32-bit values |
| `adjustment` | `dict` \| `None` | adjustment layer parameters `{"type", "key", ...}` — see [Adjustment layers](#adjustment-layers) |
| `artboard` | `dict` \| `None` | artboard (`artb`): `{"rect": (left, top, right, bottom), "preset_name", "background_type", "color"}` |
| `smart_object` | `dict` \| `None` | smart object placement (`SoLd` / `SoLE` / `PlLd`): `{"key", "uuid", "placed_id", "page", "total_pages", "anti_alias", "placed_type", "transform", "size", "filters", "linked_file"}` — `transform` is the 4 corners `(x, y)` (top-left, top-right, bottom-right, bottom-left), `linked_file` an index into `PSDFile.linked_files` |
| `vector_mask` | `dict` \| `None` | vector mask (`vmsk`, or `vsms` on shape layers): `{"key", "inverted", "not_linked", "disabled", "path"}` — see [Paths](#paths) |
| `shape` | `dict` \| `None` | shape fill / stroke / live-shape origins (`vscg` / `vstk` / `vogk`) — see [Shapes and path rasterization](#shapes-and-path-rasterization-0130) |
| `comp_states` | `dict` | per layer-comp state `{comp_id: {"enabled", "offset_x", "offset_y"}}` (empty if the layer is in no comps). `enabled` says if the layer shows in that comp — see [Layer comps](#layer-comps) |
| `info_keys` | `list[str]` | 4cc keys of every additional-layer-info block on this layer |
| `visible` | `bool` | flag bit 1 inverted |
| `transparency_protected` | `bool` | flag bit 0 |
| `obsolete` | `bool` | flag bit 2 |
| `pixel_data_irrelevant` | `bool` | flag bit 4 |

**Tip:** For Japanese PSDs prefer `name_unicode` and fall back to `name` only inside a `try / except UnicodeDecodeError`.

### `layer.text` — text-layer content & style

For text layers (`layer_type == LayerType.TEXT`) this returns a dict parsed from
the `TySh` type-tool block and its embedded Adobe *EngineData*. For every other
layer it returns `None`.

```python
{
  "text": "普通のテキスト\r二行目\r三行目",  # full string; line breaks are CR ('\r')
  "orientation": "horizontal",              # or "vertical"
  "justification": 0,                        # first paragraph: 0=left 1=right 2=center
  "transform": [xx, xy, yx, yy, tx, ty],     # affine placement transform (tx,ty = translation)
  "runs": [                                   # per-run character styling, in text order
    {
      "length": 8,                            # run length in UTF-16 code units (see note)
      "font": "NotoSansJP-Thin",              # resolved font-set family name
      "size_px": 75.0,                        # font size (px)
      "color": (1.0, 0.0, 0.0, 1.0),          # RGBA, each 0..1 (None if unspecified)
      "tracking": -100,                        # letter spacing, 1/1000 em
      "kerning": 0,                            # manual kerning
      "auto_kerning": False,                   # metrics/optical kerning enabled
      "bold": False,                           # FauxBold
      "italic": False,                         # FauxItalic
      "underline": False,                      # Underline
      "leading": None,                         # line spacing in px; None = auto
      "baseline_shift": 0.0,                   # px
      "strikethrough": False,
      "font_caps": 0,                          # 0 normal / 1 small caps / 2 all caps
      "font_baseline": 0,                      # 0 normal / 1 superscript / 2 subscript
      "horizontal_scale": 1.0,                 # 1.0 = 100%
      "vertical_scale": 1.0,
      "ligatures": True,
    },
    ...
  ],
  "paragraphs": [                             # paragraphs (split on '\r'), in text order
    {"length": 8, "justification": 0,         # length in UTF-16 code units
     "first_line_indent": 0.0, "start_indent": 0.0, "end_indent": 0.0,   # px
     "space_before": 0.0, "space_after": 0.0,                            # px
     "auto_leading": 1.2, "hyphenate": False},
    ...
  ],
  "warp": None,                               # or {"style": "warpArc", "value": 50.0,
                                              #     "horizontal_distortion", "vertical_distortion",
                                              #     "rotate": "horizontal"|"vertical"}
}
```

Lengths (`size_px`, `leading`, `baseline_shift`, indents, spacing) are in the
same units: document pixels. Values a run or paragraph inherits from the
document's default style sheet are scaled by dpi/72 like `size_px`.

Notes:
- **`length` is in UTF-16 code units**, matching Photoshop's EngineData
  `RunLengthArray`. Astral characters (e.g. emoji) count as 2. To slice the
  text by runs, index into `text.encode("utf-16-le")` (2 bytes per unit) rather
  than the Python `str` (which is code-point indexed).
- Adjacent runs may share identical styling — Photoshop stores runs as authored,
  so the run split does not always coincide with a style change.
- `color` is decoded from EngineData's `FillColor /Type 1` (RGB) and reordered
  from its on-disk `[A R G B]` to `(R, G, B, A)`. Non-RGB fill types are not yet
  decoded (`color` is `None`).
- `bold` / `italic` / `underline` are Photoshop's *faux* styles (`FauxBold`,
  `FauxItalic`, `Underline`) — the same three flags `set_run_style` writes, so
  what you set reads back here (added in 0.9.0).
- `paragraphs` splits the body on `\r`; `justification` is per paragraph, while
  the top-level `justification` is just the first paragraph's value.

```python
for layer in p.layers:
    t = layer.text
    if t is None:
        continue
    print(t["text"], "→", {r["font"] for r in t["runs"]})
```

### `layer.mask` — layer mask geometry & flags

`None` for layers without a mask. When present:

```python
{
  "top": 8, "left": 10, "bottom": 40, "right": 50,   # mask bbox on canvas
  "width": 40, "height": 32,
  "default_color": 0,        # 0..255, area outside the stored mask rect
  "flags": 0,                # raw flag byte
  "relative": False,         # bit0: position relative to layer
  "disabled": False,         # bit1: mask disabled
  "inverted": False,         # bit2: invert (obsolete)
  "from_render": False,      # bit3: mask from rendering other data
  "has_parameters": True,    # bit4: density/feather block present
  "user_density": 128,       # 0..255, or None if not stored
  "user_feather": 2.5,       # feather radius (px), or None
  "vector_density": None,    # 0..255, or None
  "vector_feather": None,    # feather radius (px), or None
  "real": None,              # or a nested dict (below) for a real/user mask
}
```

When the record carries a *real* (user + vector combined) mask (block size ≥ 36),
`real` is a dict with `flags`, `background`, and the enclosing
`top/left/bottom/right`. The mask **pixels** are unchanged — fetch them with
`p.layer_image(i, "mask")`.

`user_density`/`user_feather`/`vector_density`/`vector_feather` are only non-`None`
when `has_parameters` is set and the corresponding value was stored. (Fixed in
0.6.0: the real-mask section was previously decoded one byte off.)

### `layer.blending_ranges` — "Blend If" ranges

`None` when absent. `gray` is the composite range; `channels` has one entry per
channel. Each value is the raw 32-bit packed range (two 16-bit black/white
sub-ranges — split yourself if you need the individual sliders):

```python
{
  "gray": (65535, 65535),                 # (source, dest)
  "channels": [(0, 65535), (0, 65535), ...],
}
```

## Layer hierarchy

Layers are a **flat list in file order** — that is PSD's own encoding, and both
drawing order and the editing API (`delete_layer(i)`, `move_layer(from, to)`, …)
are defined on it. A group is a `FOLDER` layer above its contents plus a
matching `HIDDEN` layer (`</Layer group>`) below them; these are Photoshop's
`lsct` section dividers.

For walking the structure, use the **tree view** — a derived read-only view that
hides that encoding:

```python
def walk(i, depth=0):
    l = p.layers[i]
    print("  " * depth + l.name_unicode)
    for c in l.children:
        walk(c, depth + 1)

for r in p.roots:          # top-level layers, bottom-to-top
    walk(r)
```

- `p.roots` — indices of the top-level layers, bottom-to-top.
- `layer.children` — indices of that layer's direct children, bottom-to-top;
  empty for non-folders. **Dividers are left out** — they are an encoding
  artifact, not content, so walking the tree reaches every real layer exactly
  once.
- `layer.is_group` — `True` for `FOLDER` layers.
- `p.children(i)` — same as `p.layers[i].children`; `p.children(-1)` is `roots`.

Both views stay in sync: the hierarchy is recomputed after every structural edit
(add / delete / move / duplicate / copy).

`layer.parent_index` is still there (`-1` = top level, otherwise the index of
the enclosing `FOLDER` layer) if you would rather build the tree yourself. Note
it reports the divider as a child of its folder, which is why `children` filters
dividers out.

## Editing & saving

psdparse can edit a loaded PSD (or build one from scratch) and save the result.
Editable: layer **parameters** (opacity/visibility/clipping/blend/fill-opacity),
**names**, **structure** (delete/move/duplicate/cross-file copy, folder-aware
moves), **pixels** and **masks**, layer-**effect values**, and **text**
(content, per-run style, run/paragraph structure, alignment, placement and flow
box) — plus `create_blank` for new documents.

The model is *lazy*: edits only touch in-memory fields/references — nothing is
re-encoded until `save()`, which re-serializes just the parts you changed. The
original file is never touched (`load()` mmaps it read-only; `save()` writes a
new path), and an **unmodified** file still round-trips byte-identically — the
re-serialization only kicks in for layers you actually edited. Byte-exact
serializers back the effect (`lfx2`) and text (EngineData) editing, so unedited
descriptors reproduce their original bytes exactly.

Quick map of the API (details in the subsections below):

| Want to… | Use |
|---|---|
| change opacity / visibility / blend / clipping | `layer.opacity = …`, `layer.visible = …`, `layer.set_blend_mode("mul ")` |
| rename a layer | `layer.name_unicode = …` / `p.set_layer_name(i, …)` |
| change fill opacity | `layer.fill_opacity = …` |
| delete / move / duplicate | `p.delete_layer(i)` / `p.move_layer(a,b)` / `p.duplicate_layer(i)` |
| move a layer or a whole folder within its level | `p.move_layer_sibling(i, up=True)` / `p.group_span(i)` / `p.move_layer_range(...)` |
| copy a layer from another file | `p.copy_layer_from(src, j)` |
| replace layer pixels / add an image layer | `p.set_layer_pixels(...)` / `p.add_layer(...)` |
| set mask pixels + geometry / mask values | `p.set_layer_mask_pixels(...)` / `p.set_layer_mask(...)` |
| edit effect / descriptor values | `p.set_effects(i, changes)` / `p.set_layer_descriptor(...)` |
| edit text content | `p.set_text(i, str)` |
| edit a text run's style | `p.set_run_style(i, run, font=…, size_px=…, color=…, …)` |
| replace text *and* its run / paragraph structure | `p.set_rich_text(i, str, runs, paragraphs)` |
| change paragraph alignment | `p.set_justification(i, 2)` |
| list a text layer's fonts | `p.text_fonts(i)` |
| move a text layer / resize its flow box | `p.move_text_layer(i, dx, dy)` / `p.set_text_bounds(i, l,t,r,b)` |
| build a new PSD | `p.create_blank(w, h)` then `add_layer(...)` |
| write a composited preview back | `p.set_merged_image(bgra)` |
| blank the stored composite | `p.set_merged_image_solid()` |
| choose how `Txt2` is handled | `p.set_text_engine_policy(0/1/2)` / `p.drop_text_engine_data()` |

**8-bit RGB only** for the pixel/mask/new-document operations. The stored
composite image is **not** re-rendered after edits (see [Saving](#saving)); use
`set_merged_image_solid()` to blank it rather than ship a stale one.

### Parameter edits (writable properties)

```python
p = psdparse.PSDFile(); p.load("in.psd")
p.layers[3].opacity = 128          # 0..255
p.layers[3].visible = False
p.layers[3].clipping = 1
p.layers[3].set_blend_mode("mul ") # 4-char key; note trailing space on 3-letter keys
p.layers[3].name_unicode = "新しい名前"   # rename (also: p.set_layer_name(3, "..."))
p.save("out.psd")
```

`opacity` / `clipping` / `visible` / `blend_mode_key` are record-level fields,
re-serialized directly. The rest edit the **extra-data block**, which is
reconstructed on save (the layer mask and blending ranges are copied through
byte-for-byte unless you edit the mask itself):

```python
p.layers[3].name_unicode = "新しい名前"      # rename (or p.set_layer_name(3, ...))
p.layers[3].fill_opacity = 128               # 0..255 (the 'iOpa' block)

# edit an existing mask's values (the layer must already have a mask;
# the mask rectangle and pixels are unchanged):
p.set_layer_mask(3, disabled=True, density=200, feather=2.5, default_color=0)
```

`set_layer_mask` takes any subset of `disabled` (bool), `density` (0..255),
`feather` (px), `default_color` (0..255). To change the mask **geometry**
(rectangle) or its pixels, use `set_layer_mask_pixels(...)`
(see [Mask pixels & geometry](#mask-pixels--geometry)).

### Effect / descriptor values (E3)

Layer effects (`lfx2`) and descriptor-based fill layers are edited by passing a
**partial** dict of changes, shaped like `layer.effects` (see
[Descriptor blocks](#descriptor-blocks)). Only the leaf values you include are
overwritten; structure, class IDs, types and every untouched value are preserved
(the descriptor is re-serialized byte-for-byte apart from your edits).

```python
# drop-shadow opacity 100 -> 50 %, turn all effects off:
p.set_effects(i, {
    "patternFill": {"Opct": {"value": 50.0}, "enab": False},
    "masterFXSwitch": False,
})

# generic form for any descriptor key (fill layers etc.):
p.set_layer_descriptor(i, "SoCo", {"Clr ": {"Rd  ": 255.0, "Grn ": 0.0, "Bl  ": 0.0}})
```

Value mapping when merging: numbers → Integer/Double; `{"value": ..}` (or a bare
number) → UnitFloat; bool → Boolean; str → String; `{"value": ..}` or str →
Enumerated; nested dict → sub-descriptor (recurse); list → per-index. Unknown
keys are ignored, and only *existing* keys are edited (you cannot add new effect
fields this way). `layer.descriptor_bytes(key)` returns a block's raw bytes for
inspection.

### Structural edits (methods on `PSDFile`)

```python
p.delete_layer(i)                  # remove layer i
p.move_layer(from_i, to_i)         # reorder (to_i = index in the post-removal list)
new_i = p.duplicate_layer(i)       # copy layer i, inserted right after it
new_i = p.copy_layer_from(src, j, dest_index=-1)  # copy layer j from another PSDFile

# wrap layers[from_index : from_index+count] in a group;
# returns the folder layer's index
f_i = p.add_folder("group name", from_index, count,
                   closed=False, blend_mode="pass", opacity=255)
```

Notes:
- **`add_folder` inserts the two marker layers PSD uses for a group**: a
  `</Layer group>` divider (`LayerType.HIDDEN`) below the contents and the
  folder layer (`LayerType.FOLDER`) above them, both with an empty (0×0) rect.
  Nest groups by wrapping an outer range that already contains inner ones.
  `blend_mode` defaults to `"pass"` (pass-through), matching Photoshop's
  new-group default; `count=0` creates an empty folder at `from_index`.
  Indices below `from_index` are unaffected, so building bottom-up and wrapping
  each group as soon as its contents are in place needs no index bookkeeping.
- **`move_layer` moves one entry of the flat list.** Moving a `FOLDER` layer
  does *not* drag its divider and contents along — the group comes apart. Use
  the folder-aware moves below for whole groups.
- **`layer.parent_index` is refreshed after every structural edit** (delete /
  move / duplicate / copy / add re-link the hierarchy, 0.9.0), so the tree you
  read back matches the edited list. Before 0.9.0 it kept the values computed at
  load time and went stale.
- `duplicate_layer` / `copy_layer_from` assign the copy a **fresh `layer_id`**
  (max existing lyid + 1, like Photoshop) so layer IDs stay unique within the
  document; the `lyid` additional-info block is rewritten on save.
- `delete_layer` / `duplicate_layer` on a single layer are exact. Deleting **one
  half of a group's FOLDER/HIDDEN divider pair unbalances the group** — delete
  whole groups (both dividers + contents) for clean nesting. (Unbalanced results
  still load; the hierarchy just looks odd.)
- **`copy_layer_from` copies across files.** The copied layer references the
  *source's* pixel/extra bytes lazily, so **the source `PSDFile` must stay open
  until the destination is saved** (and, in fact, until it is garbage-collected).
  psdparse keeps a reference to the source automatically, so simply saving before
  discarding both is enough. Source and destination must share color mode and bit
  depth.
- **New-from-scratch** documents (`create_blank`) and **text content** editing
  (`set_text`) are covered in their own subsections below.
- The stored **composite (merged) image is not regenerated** after edits — it
  stays as it was until Photoshop (or another editor) recomposites on open.

### Folder-aware moves

```python
start, count = p.group_span(i)         # the block layer i occupies
new_i = p.move_layer_sibling(i, up=True)   # -> int, or None if it can't move
p.move_layer_range(start, count, to_index)
```

- **`group_span(i)`** returns `(start, count)`. For a `FOLDER` that is the whole
  `[HIDDEN divider … FOLDER]` block including nested groups; for anything else
  it is `(i, 1)`.
- **`move_layer_sibling(i, up=True)`** swaps the layer with its next sibling
  *at the same level*. `up=True` is one step up in Photoshop's layer panel
  (later in the flat list). A folder travels with its contents and steps over a
  sibling folder in one move; the move never crosses into another folder.
  Returns the layer's new index, or `None` when it is already at the end of its
  level (nothing moves in that case).
- **`move_layer_range(from_index, count, to_index)`** is the low-level block
  move; `to_index` is an index in the list *before* removal. Moving a range
  onto itself is a no-op. Raises `IndexError` on bad arguments.

```python
# move a folder (with everything inside it) to the bottom of the document
start, count = p.group_span(folder_index)
p.move_layer_range(start, count, 0)
```

### Pixel edits (E4)

Replace an existing layer's pixels, or add a whole new image layer, from BGRA
bytes (the same interleave `layer_image` returns). **8-bit RGB documents only.**

```python
# replace layer i's pixels (left/top kept; width/height updated)
p.set_layer_pixels(i, bgra_bytes, width, height)

# add a new image layer; returns its index
new_i = p.add_layer("name", left, top, bgra_bytes, width, height,
                    blend_mode="norm", opacity=255, dest_index=-1)
```

- `bgra_bytes` must be exactly `width*height*4` bytes (B, G, R, A per pixel).
- Channels are PackBits(RLE)-encoded on `save()`; decode round-trips exactly.
- `add_layer` writes the name as both a Pascal string and a Unicode `luni`
  block, so non-ASCII names (incl. emoji) survive.
- `set_layer_pixels` keeps an existing **mask channel** intact (only the colour
  channels are rebuilt). Replacing a *masked* layer at a different size leaves a
  stale mask rectangle, though — prefer same-size replacement, or follow with
  `set_layer_mask_pixels` to reset the mask.

### Mask pixels & geometry

```python
# set/replace the mask with a grayscale buffer (0 = hidden, 255 = shown),
# positioned at (left, top). Creates the mask if the layer had none:
p.set_layer_mask_pixels(i, gray_bytes, top, left, width, height)
```

- `gray_bytes` is `width*height` bytes (one per pixel). This sets both the mask
  pixels **and** the mask rectangle (geometry). Colour channels are preserved.
- Mask **value** attributes (disabled / density / feather / default colour) are
  edited separately with `set_layer_mask(...)`. 8-bit documents only.

### Text content (E6)

Replace a text layer's body text. This rewrites the embedded Adobe *EngineData*
(re-serialized byte-for-byte in Photoshop's format) plus the `Txt ` descriptor
string, and re-serializes the `TySh` block keeping the warp/bounds intact.

```python
p.set_text(i, "新しいテキスト\r二行目")     # \r separates lines
p.save("out.psd")
```

- Non-ASCII and emoji are supported (stored as UTF-16 in EngineData).
- A trailing newline (`\r`) is added if missing (Photoshop's convention).
- **`set_text` collapses styling to the first run's style** (single style run
  over the new text). To keep per-run styling, edit runs individually with
  `set_run_style` (below) instead of changing the text.
- Only the *content* changes; the layer's transform, font set and bounds are
  kept. Raises for non-text layers.
- The document-wide `Txt2` block is kept in step — **without this the edit is
  invisible in Photoshop**. See [Txt2](#txt2-document-wide-text-engine-data).

Edit an existing run's style in place (text and run lengths unchanged):

```python
# run indices match layer.text["runs"]
p.set_run_style(i, run_index=0, size_px=48.0, color=(1.0, 0.0, 0.0))   # 48px, red
p.set_run_style(i, run_index=1, tracking=100, bold=True, underline=True)
p.set_run_style(i, run_index=1, font="Arial")
```

- Any subset of: `font` (str), `size_px` (float), `color` ((r,g,b) or
  (r,g,b,a), each 0..1), `tracking` / `kerning` (int), `bold` / `italic` /
  `underline` (bool).
- Keys are added to the run if it inherited them from the default style sheet.
- `font` is matched against the document's font set (`p.text_fonts(i)`) and
  **appended to it** when the name isn't there yet. The name is the PostScript-ish
  font-set name Photoshop stores (`"NotoSansJP-Regular"`, `"Arial"`), not a
  display name — Photoshop resolves it when it opens the file, so a name the
  machine doesn't have falls back to a substitute font.

```python
p.text_fonts(i)   # -> ['NotoSansJP-Regular', 'HGPKyokashotai', 'AdobeInvisFont', ...]
```

### `Txt2` (document-wide Text Engine Data)

Photoshop CS3 and later keep the whole document's text engine state in a `Txt2`
block at the end of the layer & mask section, and **read it in preference to
each layer's `TySh`**. Rewriting `TySh` alone therefore leaves the edit invisible
in Photoshop: it shows the *old text*, not merely a stale raster. psdparse keeps
the two in step for you.

```python
p.set_text(i, "new body")
p.text_engine_texts()          # -> ['new body\r', ...] in TextIndex order
p.has_text_engine_data()       # -> True
p.text_engine_dropped          # -> False (nothing had to be given up)
```

The default policy mirrors what it can and drops the block when it cannot:

```python
p.set_text_engine_policy(0)    # SYNC   (default)
p.set_text_engine_policy(1)    # REMOVE — always drop it, TySh wins
p.set_text_engine_policy(2)    # KEEP   — leave it alone (edits stay invisible)
p.drop_text_engine_data()      # drop it now
```

- **Body text and run lengths are mirrored.** Growing, shrinking and changing the
  number of paragraphs are all fine; Photoshop recomputes its own layout cache.
- **Style, alignment, text box and placement are not.** Mirroring them would mean
  rebuilding `Txt2`'s own style sheets (numeric key aliases), so those edits drop
  the block instead — Photoshop then reads `TySh`, which is correct. Ask with
  `p.text_engine_dropped` afterwards.
- `set_rich_text(..., formatting_unchanged=True)` is the escape hatch for callers
  that always pass **absolute** styles (so "no field given" doesn't mean "keep
  what was there"): they cannot signal a text-only edit through the presence of
  style fields, so they declare it.
- `p.layer_text_index(i)` is the layer's position within `Txt2` (its `TySh`
  `TextIndex`).

Note that **Photoshop does not re-render a text layer when it opens a file**, so
the layer's raster stays as it was until Photoshop redraws it. Only Photoshop can
produce that picture; psdtext ships a `tools/update-text-layers.jsx` that makes it
redraw every text layer.

### Rich text (runs & paragraphs)

`set_text` collapses the whole layer to one style run. To change the text *and*
keep (or rebuild) per-part formatting, use `set_rich_text`:

```python
p.set_rich_text(i, "赤い字\r青い字",
                runs=[{"length": 4, "color": (1.0, 0.0, 0.0), "size_px": 40.0},
                      {"length": 4, "color": (0.0, 0.0, 1.0), "bold": True}],
                paragraphs=[{"length": 4, "justification": 0},
                            {"length": 4, "justification": 2}])
```

- **`runs`** — a list of dicts, each with `length` (UTF-16 code units,
  **required**) plus any of the `set_run_style` style keys (`font`, `size_px`,
  `color`, `tracking`, `kerning`, `bold`, `italic`, `underline`).
- Every run starts from the **original first run as a template**; only the keys
  you give are overridden, so unspecified formatting keeps the layer's look.
- If the run lengths don't add up to the text length, the **last run absorbs
  the difference** (extended or truncated); zero-length runs are dropped.
  Omitting `runs` (or passing `[]`) collapses to a single run, like `set_text`.
- **`paragraphs`** — a list of `{"length": int, "justification": int}`
  (0=left, 1=right, 2=center), optionally with the paragraph keys of
  `set_paragraph_style` below; same length-absorbing rule. Omitting it collapses
  to a single paragraph. Run dicts likewise accept the keyword-only keys of
  `set_run_style` (`leading`, `baseline_shift`, `strikethrough`, …).
- A trailing `\r` is appended if missing (Photoshop's convention), so
  `length` accounting should include it — or just let the last run absorb it.
- Lengths are **UTF-16 code units**, so astral characters (emoji) count as 2:
  `len(part.encode("utf-16-le")) // 2`.

Alignment alone, without touching text or runs:

```python
p.set_justification(i, 2)                  # every paragraph -> center
p.set_justification(i, 0, para_index=1)    # only the 2nd paragraph -> left
```

Other paragraph values (px unless noted; only the ones you pass change):

```python
p.set_paragraph_style(i, 0, first_line_indent=12, start_indent=4, end_indent=6,
                      space_before=8, space_after=10,
                      auto_leading=1.5,      # multiplier used by auto leading
                      hyphenate=True, justification=2)
p.set_paragraph_style(i, space_after=6)      # para_index=-1 (default): every paragraph
```

Character values beyond the original set are keyword-only on `set_run_style`:

```python
p.set_run_style(i, 0, leading=40)            # px; turns auto leading off
p.set_run_style(i, 0, leading="auto")        # back to auto
p.set_run_style(i, 0, baseline_shift=-3, strikethrough=True, font_caps=2,
                font_baseline=1, horizontal_scale=1.25, vertical_scale=0.9,
                ligatures=False)
```

After any text edit, `layer.text` is re-read from the rewritten data, so it
shows the new values without reloading the file.

### Text placement & text box

```python
p.text_transform(i)          # -> (xx, xy, yx, yy, tx, ty), same as text["transform"]
p.set_text_transform(i, (1, 0, 0, 1, 111.0, 222.0))
p.move_text_layer(i, dx, dy)
p.text_bounds(i)             # -> (left, top, right, bottom)
p.set_text_bounds(i, 0, 0, 300, 200)
```

- **`move_text_layer(i, dx, dy)`** is the one to use for plain translation: it
  shifts the transform's `tx`/`ty` **and** the layer rectangle (and the mask
  rectangle, if any), so the PSD's baked raster travels with the text instead of
  being left behind. `set_text_transform` only rewrites the `TySh` block.
- **`text_bounds` / `set_text_bounds`** are the descriptor's `bounds` — the flow
  box, in the transform's **local** coordinates (add `tx`/`ty` for canvas
  coordinates). Only paragraph (box) text actually re-flows into a new box; for
  point text Photoshop rebuilds the box from the glyphs on open. The stored
  `boundingBox` is clamped into the new box so the layer still displays sanely.
- psdparse does not re-render text — the layer's pixels are whatever Photoshop
  last baked. Moving or re-flowing shows up properly once Photoshop reopens the
  file.

### New from scratch (E5)

Build a PSD without loading one first:

```python
p = psdparse.PSDFile()
p.create_blank(1024, 768)                       # blank 8-bit RGB, white composite
p.add_layer("background", 0, 0, bg_bgra, 1024, 768)
p.add_layer("sprite", 100, 100, sprite_bgra, 200, 150, "norm", 255)
p.save("new.psd")
```

- `create_blank(width, height, mode=COLOR_MODE_RGB)` — 8-bit RGB only. Resets the
  object to an empty document with a white stored composite.
- Add content with `add_layer(...)`; a document with zero layers is also valid.
- As with edited files, the stored composite is **not** rendered from the layers
  — it stays white until an editor recomposites on open.

### Saving

The stored **composite (merged) image** is not re-rendered after edits. If you
composite the layers yourself (e.g. with Pillow — see [`examples/`](../examples/)),
write the result back as the PSD's preview with:

```python
p.set_merged_image(bgra_bytes)   # canvas-sized BGRA (header.width*header.height*4)
```

If you cannot recomposite — text layers can only be rendered by Photoshop — blank
it instead of shipping a picture that is no longer true:

```python
p.set_merged_image_solid()       # white by default; RLE-compressed, so it stays
                                 # small on any canvas
```

This is what Photoshop itself writes with "Maximize PSD compatibility" off.
Photoshop composites from the layers, so what it displays is unaffected.

`save(path)` returns `True`/`False`. **Do not save over a file that is currently
loaded** (by this or any live `PSDFile`): `load()` memory-maps the file
read-only, so the write is refused and `save()` returns `False` (the original is
never corrupted). Always save to a fresh path, then swap the files yourself if
you want to replace the original.

```python
# Merge one layer from file B into file A, on top:
a = psdparse.PSDFile(); a.load("A.psd")
b = psdparse.PSDFile(); b.load("B.psd")
a.copy_layer_from(b, 0)            # append B's layer 0
a.save("merged.psd")              # b is kept alive until here
```

## Document resources

Read-only accessors on `PSDFile` for whole-document metadata. Each returns
`None` (or an empty list) when the PSD lacks that resource.

```python
p.guides        # dict|None : {"horizontal_grid", "vertical_grid", "guides":[{"location","direction"}]}
p.paths         # list[dict]: saved paths (2000-2997) and the work path (1025) — see Paths below
p.annotations   # list[dict]: notes ('Anno'): kind, open, icon_rect, popup_rect, color, author, name, mod_date, text
p.alpha_channels # list[dict]: extra channels after the color channels (alpha / spot / merged transparency): plane, name, kind, color_space, color, opacity
p.merged_channel(plane)  # bytes: one merged-image channel as 8-bit gray (width*height)
p.patterns      # list[dict]: document patterns (Patt/Pat2/Pat3): id, name, mode, width, height
p.pattern_image(i_or_id) # (bgra_bytes, width, height) | None
p.linked_files  # list[dict]: smart-object source files (lnk2/lnk3/lnkD/lnkE): kind, uuid, name, file_type, size, has_data
p.linked_file_data(i_or_uuid)  # bytes|None: the embedded file (or the cached copy of an external one)
p.slices        # dict|None : {"group_name", "bounding":{...}, "slices":[{...}]}
p.layer_comps   # list[dict]: [{"id","name","comment","record_visibility","record_position","record_appearance"}]
p.color_table   # dict|None : {"colors":[(r,g,b,a)], "valid_count", "transparency_index"} for indexed-color PSDs
p.global_layer_mask  # dict|None : {"overlay_color_space", "color":(c1,c2,c3,c4), "opacity", "kind"}
```

### Layer comps

`p.layer_comps` lists the document's layer comps (saved layer-state snapshots).
Which layers each comp shows is on the **layers**: `layer.comp_states` maps a
comp id to that layer's state in the comp:

```python
{ comp_id: {"enabled": True, "offset_x": 0, "offset_y": 0}, ... }
```

`enabled` is whether the layer is visible in that comp; a layer the comp doesn't
mention isn't in the dict (fall back to its current visibility). To render a comp
(visibility only — position/appearance overrides aren't applied):

```python
for comp in p.layer_comps:
    show = {i for i, l in enumerate(p.layers)
            if (l.comp_states[comp["id"]]["enabled"]
                if comp["id"] in l.comp_states else l.visible)}
    # composite `show` with Pillow — see examples/variations.py (composite_comp)
```

`examples/variations.py --comps` renders every comp this way.

### Image resources (raw)

Most image resources are exposed as their raw on-disk bytes; decoding (EXIF
tags, rendering the thumbnail, parsing the ICC profile) is left to the caller.

```python
p.image_resource_ids     # list[int]  : IDs of every resource present
p.image_resource(id)     # bytes|None : raw bytes of the resource with that ID
p.icc_profile            # bytes|None : ICC profile (resource 1039)
p.exif                   # bytes|None : EXIF block (1058)
p.xmp                    # str|None   : XMP packet (1060), UTF-8 XML
p.thumbnail              # dict|None  : {"format","width","height","bits","resource_id","data"}
```

- **`thumbnail`** — resource 1036 (RGB) or legacy 1033 (BGR). When
  `format == "jpeg"`, `data` is JFIF JPEG bytes ready for `PIL.Image.open`:

  ```python
  import io
  from PIL import Image
  th = p.thumbnail
  if th and th["format"] == "jpeg":
      Image.open(io.BytesIO(th["data"])).save("thumb.png")
  ```

- **`xmp`** decodes as UTF-8 `str`; if a file's packet is not valid UTF-8, read
  the raw bytes with `p.image_resource(1060)` instead.

### Adjustment layers

`layer.adjustment` is `None` or a dict with `"type"`, `"key"` (the block's 4cc)
and the values below. Values are as stored (integers unless noted).

| type | values |
|---|---|
| `levels` | `records`: rows `[input_black, input_white, output_black, output_white, gamma]` (gamma as float). Row 0 is the composite, then one row per channel (29 or more rows) |
| `curves` | `channels`: channel index per curve (0 = composite); `points`: per curve a list of `(input, output)` pairs, or `maps` (256 values each) for map-style curves |
| `hue_saturation` | `colorize`, `colorization` `[hue, saturation, lightness]`, `master` `[hue, saturation, lightness]`, `ranges`: 6 rows `[range1..4, hue, saturation, lightness]` |
| `color_balance` | `shadows` / `midtones` / `highlights` `[cyan_red, magenta_green, yellow_blue]`, `preserve_luminosity` |
| `brightness_contrast` | `brightness`, `contrast`, `mean`, `lab_only`; newer files also carry the real values in `descriptor` (`CgEd`) |
| `selective_color` | `method` (0 relative / 1 absolute), `records`: 10 rows `[cyan, magenta, yellow, black]` (row 0 unused, then reds, yellows, greens, cyans, blues, magentas, whites, neutrals, blacks) |
| `threshold` / `posterize` | `level` / `levels` |
| `invert` | — |
| `channel_mixer` | `monochrome`, `channels`: rows `[red, green, blue, extra, constant]` per output channel |
| `photo_filter` | `version`; `color_space` + `color` (version 2) or `xyz` (version 3); `density`, `preserve_luminosity` |
| `exposure` | `exposure`, `offset`, `gamma` (floats) |
| `gradient_map` | `name`, `reversed`, `dithered`, `color_stops` rows `[location, midpoint, kind, c0, c1, c2, c3]`, `transparency_stops` rows `[location, midpoint, opacity]`, plus `interpolation`, `method` (version 3), noise settings |
| `vibrance` / `black_white` / `color_lookup` | `descriptor` (the block's descriptor as a dict) |

`"incomplete": True` is added when a binary block ended early (the values read so
far are kept). Read-only for now; saving keeps the original bytes.

### Paths

`layer.vector_mask["path"]` and each `PSDFile.paths[i]["path"]` share one shape:

```python
{"subpaths": [{"closed": True, "operation": 1, "index": 0,
               "knots": [{"anchor": (x, y), "preceding": (x, y), "leaving": (x, y),
                          "linked": True}, ...]}, ...],
 "initial_fill": 0,        # initial fill rule record, None if absent
 "clipboard": None}        # or {"top", "left", "bottom", "right", "resolution"}
```

Points are `(x, y)` in document pixels (stored as 8.24 fixed-point fractions of
the canvas). `preceding` is the control point on the way into the anchor,
`leaving` the one on the way out. `operation` is how a subpath combines with
the ones before it: `-1`/`1` combine, `2` subtract, `3` intersect, `0` exclude.
`PSDFile.paths` entries are `{"id", "kind": "saved"|"work", "name": bytes,
"unicode_name": str|None, "path"}` — `name` is the raw Pascal resource name (system
encoding, e.g. Shift-JIS), `unicode_name` comes from the document's `pths` block.
Read-only for now; saving keeps the original bytes.

### Shapes and path rasterization (0.13.0)

`layer.shape` gathers what a shape layer keeps besides its path (`None` when the
layer has none of `vscg` / `vstk` / `vogk`):

```python
{"fill_enabled": True, "stroke_enabled": True,
 "fill": {"kind": "solid" | "gradient" | "pattern", "descriptor": {...}},   # 'vscg', or None
 "stroke": {"width": 4.0,              # px ('pt' widths converted with the document dpi)
            "alignment": "inside" | "center" | "outside",
            "cap": "butt" | "round" | "square", "join": "miter" | "round" | "bevel",
            "miter_limit": 100.0,      # ratio of the line width
            "dashes": [8.0, 4.0], "dash_offset": 0.0,   # px (stored as multiples of the width)
            "opacity": 1.0, "blend_mode": "normal",
            "content_kind": "solid", "content": {...}},  # 'vstk', or None
 "origins": [{"type": "rectangle" | "rounded_rectangle" | "line" | "ellipse" | None,
              "type_id": 1, "index": 0,          # index = the subpath's 'index'
              "box": (l, t, r, b), "radii": (tl, tr, br, bl) | None,
              "line": (x0, y0, x1, y1) | None, "line_weight": float | None,
              "invalidated": False}],            # 'vogk' live-shape origins
 "path": {...}}                        # the vector mask path (same shape as above)
```

```python
p.shape_mask(i, part="both") -> (bytes, left, top, width, height) | None
```

Rasterizes the layer's path to 8-bit anti-aliased coverage: `part="fill"` (the
path area, inverted masks included), `"stroke"` (the `vstk` stroke with its
width, alignment, caps, joins and dashes) or `"both"`. The rectangle holds the
path and its stroke in document pixels and may extend past the canvas. Mask
density / feather are not applied.

Paths from anywhere — `layer.vector_mask["path"]`, `PSDFile.paths[i]["path"]`,
or your own list of subpaths (knots may be plain `(x, y)` tuples for straight
segments; `closed` defaults to True, `operation` to -1) — can be flattened or
rasterized with module functions:

```python
psdparse.flatten_path(path, tolerance=0.1)
# -> [{"closed", "operation", "points": [(x, y), ...]}, ...]
psdparse.rasterize_path(path, width, height, left=0, top=0) -> bytes
psdparse.stroke_path(path, width, height, left=0, top=0, line_width=1.0,
                     alignment="center", cap="butt", join="miter",
                     miter_limit=4.0, dashes=(), dash_offset=0.0) -> bytes
```

The raster is `width x height` bytes whose top-left sits at `(left, top)` in
path coordinates. Subpaths combine like Photoshop's shape operations
(`-1` joins the previous subpath, `0` exclude, `1` combine, `2` subtract,
`3` intersect) and `initial_fill: 1` starts from a filled raster. `inside` /
`outside` alignment puts the whole width on one side of closed subpaths. The
same routines are in the C API (`psdfx_flatten_subpath`, `psdfx_fill_path`,
`psdfx_stroke_path`), and `composite()` / `render_layer()` use them to draw
shape strokes.

- **`guides`** — grid spacing (in 1/32 px) and each guide's `location` (1/32 px
  from origin) and `direction` (`"vertical"` / `"horizontal"`).
- **`slices`** — Photoshop slices (v6). Each slice has `id`, `name`, bbox
  (`left/top/right/bottom`), `url`, `target`, `message`, `alt_tag`, `cell_text`,
  alignment, and an `(r, g, b, a)` `color` tuple.
- **`color_table`** — only present for `COLOR_MODE_INDEXED` PSDs; `colors` is the
  palette and `transparency_index` is `-1` when there is no transparent entry.

## Descriptor blocks

Photoshop stores layer **effects**, **fill-layer** content and many other
tagged blocks as its generic *descriptor* tree (the same OSType structure used
throughout PSD). These accessors decode a block into nested Python
dicts/lists so they can be read without a decoder per feature.

```python
layer.effects   # dict|None : object-based effects ('lfx2')
layer.fill      # dict|None : {"type": "solid"|"gradient"|"pattern", "data": {...}}
layer.info_keys # list[str] : every additional-info 4cc key present on the layer
layer.descriptor(key, skip=-1)  # dict|None : parse an arbitrary key as a descriptor
```

**Value mapping** (descriptor item → Python):

| Descriptor type | Python |
|---|---|
| Integer / Double | `int` / `float` |
| Boolean | `bool` |
| String / Alias / Class | `str` |
| UnitFloat | `{"value": float, "unit": str}` (unit: `percent`, `angle`, `pixels`, …) |
| Enumerated | `{"type": str, "value": str}` |
| Descriptor (nested) | `dict` (keys are raw 4cc, **may end in a space**, e.g. `"Scl "`) |
| List | `list` |
| RawData (`tdta`) | `bytes` |
| LargeInteger (`comp`) | `int` |
| UnitFloats (`UnFl`) | `{"values": list[float], "unit": str}` |
| ObjectArray (`ObAr`) | `dict` (like a nested descriptor; its values are usually `UnFl`) |
| Reference / unknown | `None` |

```python
fx = layer.effects
if fx:
    print("effects on:", fx.get("masterFXSwitch"))
    po = fx.get("patternFill")           # a nested descriptor dict
    if po:
        print("pattern overlay opacity:", po["Opct"]["value"])   # -> 100.0
```

Notes:
- Keys are the **raw 4cc** as stored (trailing spaces preserved) — index with
  the exact string, e.g. `fx["Scl "]`, `color["Rd  "]`.
- `layer.effects` is `lfx2` (object-based, Photoshop 6+). The older binary
  `lrFX` block is **not** a descriptor and returns `None` via `descriptor()`.
- `descriptor(key, skip)` is the generic escape hatch: `skip` is the number of
  version-prefix bytes before the descriptor (`-1` auto-detects for the known
  descriptor keys: `lfx2` = 8, `SoCo`/`GdFl`/`PtFl`/`vstk`/`CgEd` = 4,
  `vscg`/`vogk` = 8, `SoLd` = 12, otherwise 0). Use `info_keys` to discover which
  blocks a layer carries. The smart-object/vector defaults (`SoLd`/`vstk`/`vscg`/
  `vogk`) are set from the psd-tools layouts but not yet verified against a real
  smart-object sample — override `skip` if a parse looks wrong.
- Decoding is **lazy** — the descriptor is parsed from the block's raw bytes on
  each access, so cache the result if you read it repeatedly.

## Enums

```python
psdparse.LayerType.NORMAL
psdparse.LayerType.HIDDEN
psdparse.LayerType.FOLDER
psdparse.LayerType.ADJUST
psdparse.LayerType.FILL
psdparse.LayerType.TEXT
```

```python
psdparse.BlendMode.NORMAL
psdparse.BlendMode.MULTIPLY
psdparse.BlendMode.SCREEN
psdparse.BlendMode.OVERLAY
psdparse.BlendMode.PASS_THROUGH
# ... 28 values total. Use BlendMode.<name>.value to get the int.
```

Module-level integer constants for direct comparison:

```python
psdparse.COLOR_MODE_BITMAP, COLOR_MODE_GRAYSCALE, COLOR_MODE_INDEXED,
COLOR_MODE_RGB, COLOR_MODE_CMYK, COLOR_MODE_MULTICHANNEL,
COLOR_MODE_DUOTONE, COLOR_MODE_LAB

psdparse.LAYER_TYPE_NORMAL, LAYER_TYPE_HIDDEN, LAYER_TYPE_FOLDER,
LAYER_TYPE_ADJUST, LAYER_TYPE_FILL
```

Pixel extraction (`merged_image()` / `layer_image()`) supports Bitmap, Grayscale,
RGB, Indexed, CMYK (→RGB), **Duotone** (rendered as grayscale) and **Lab**
(standard D65 CIELAB→sRGB approximation — Photoshop uses D50, so highly saturated
colors differ slightly). **Multichannel** has no canonical RGB mapping and is not
rendered.

## Pixel format

All `*_image()` methods return interleaved BGRA in little-endian byte order:

```
byte 0: B  (blue)
byte 1: G  (green)
byte 2: R  (red)
byte 3: A  (alpha)
```

This matches PIL's `"BGRA"` raw decoder:

```python
from PIL import Image
img = Image.frombytes("RGBA", (w, h), bgra_bytes, "raw", "BGRA")
img.save("out.png")
```

For NumPy:

```python
import numpy as np
arr = np.frombuffer(bgra_bytes, dtype=np.uint8).reshape(h, w, 4)
# arr[..., [0,1,2,3]] is B, G, R, A
```

## Error model

- Invalid paths: `load()` / `load_streamed()` return `False` (no exception).
- Invalid PSD data: `load*()` returns `False` and the object is left empty (`is_loaded == False`).
- Out-of-range `layer_image(index)`: raises `IndexError`.
- Bad `mode` string: raises `ValueError`.
- Reading `name` on a non-UTF-8 byte sequence: raises `UnicodeDecodeError` — see the tip above.

## Memory model

- `load()` mmaps the file. The mapping is held by the `PSDFile` instance; it is unmapped when the instance is destroyed or a new file is loaded.
- `load_streamed()` keeps a `std::ifstream` alive in `PSDFile`. Closed on destruction / re-load.
- `load_bytes()` copies the input into an internal vector. The Python `bytes` can go out of scope safely.
- Layer pixel decoding allocates a fresh BGRA buffer of `4 * w * h` bytes per call. There's no caching — call once and hold the result if you need it twice.

## Worked example: export every layer to PNG

```python
import psdparse
from PIL import Image

p = psdparse.PSDFile()
assert p.load("file.psd")

for i, layer in enumerate(p.layers):
    if layer.layer_type != psdparse.LayerType.NORMAL: continue
    if layer.width == 0 or layer.height == 0:        continue
    name = layer.name_unicode or f"layer_{i}"
    img = Image.frombytes(
        "RGBA", (layer.width, layer.height),
        p.layer_image(i, "masked"), "raw", "BGRA"
    )
    img.save(f"{i:03d}_{name}.png")
```

See `tools/psd_export.py` for a more complete extraction tool that also dumps metadata as JSON.
