# psdparse Python API Reference

The Python bindings expose a small surface area focused on **reading** PSDs, **extracting raw pixel data**, and **saving** a loaded file back as PSD. Pixel data is returned as raw BGRA `bytes` objects suitable for handing to PIL, NumPy, etc.

All public path arguments are Python `str`. pybind11 transparently encodes them as UTF-8 before reaching the C++ layer.

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
p.header.version     # int (1 or 2)
```

### Layers

```python
p.layers             # list[LayerInfo] -- read-only
p.merged_alpha       # bool
p.is_loaded          # bool
```

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
| `text` | `dict` \| `None` | text-layer content & style (`None` for non-text layers) — see below |
| `mask` | `dict` \| `None` | layer mask geometry & flags (`None` when the layer has no mask) — see below |
| `blending_ranges` | `dict` \| `None` | "Blend If" ranges (`None` when absent) — see below |
| `effects` | `dict` \| `None` | layer effects (`lfx2`) as a descriptor dict — see [Descriptor blocks](#descriptor-blocks) |
| `fill` | `dict` \| `None` | fill-layer content (solid/gradient/pattern) — see [Descriptor blocks](#descriptor-blocks) |
| `sheet_color` | `dict` \| `None` | layer-panel color label (`lclr`): `{"index", "name"}` — `None` when no `lclr` block |
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
      "size_px": 75.0,                        # font size (pt)
      "color": (1.0, 0.0, 0.0, 1.0),          # RGBA, each 0..1 (None if unspecified)
      "tracking": -100,                        # letter spacing, 1/1000 em
      "kerning": 0,                            # manual kerning
      "auto_kerning": False,                   # metrics/optical kerning enabled
    },
    ...
  ],
}
```

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

Layers are a **flat list in file order**; folder structure is recovered from
`layer.parent_index` (`-1` = top level, otherwise the index of the enclosing
`FOLDER` layer). To build a tree:

```python
children = {i: [] for i in range(len(p.layers))}
roots = []
for i, l in enumerate(p.layers):
    (roots if l.parent_index == -1 else children[l.parent_index]).append(i)
```

`FOLDER` marks a group's start and the matching `HIDDEN` layer marks its end
(these are Photoshop's `lsct` section dividers).

## Editing & saving

psdparse can edit a loaded PSD (or build one from scratch) and save the result.
Editable: layer **parameters** (opacity/visibility/clipping/blend/fill-opacity),
**names**, **structure** (delete/move/duplicate/cross-file copy), **pixels** and
**masks**, layer-**effect values**, and **text content** — plus `create_blank`
for new documents.

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
| copy a layer from another file | `p.copy_layer_from(src, j)` |
| replace layer pixels / add an image layer | `p.set_layer_pixels(...)` / `p.add_layer(...)` |
| set mask pixels + geometry / mask values | `p.set_layer_mask_pixels(...)` / `p.set_layer_mask(...)` |
| edit effect / descriptor values | `p.set_effects(i, changes)` / `p.set_layer_descriptor(...)` |
| edit text content | `p.set_text(i, str)` |
| edit a text run's style | `p.set_run_style(i, run, size_px=…, color=…, …)` |
| build a new PSD | `p.create_blank(w, h)` then `add_layer(...)` |
| write a composited preview back | `p.set_merged_image(bgra)` |

**8-bit RGB only** for the pixel/mask/new-document operations. The stored
composite image is **not** re-rendered after edits (see [Saving](#saving)).

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
```

Notes:
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

Edit an existing run's style in place (text and run lengths unchanged):

```python
# run indices match layer.text["runs"]
p.set_run_style(i, run=0, size_px=48.0, color=(1.0, 0.0, 0.0))   # 48px, red
p.set_run_style(i, run=1, tracking=100, bold=True, underline=True)
```

- Any subset of: `size_px` (float), `color` ((r,g,b) or (r,g,b,a), each 0..1),
  `tracking` / `kerning` (int), `bold` / `italic` / `underline` (bool).
- Keys are added to the run if it inherited them from the default style sheet.
- **Changing the font by name is not supported** (would require editing the
  document's font set); re-splitting text into new runs isn't either.

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
